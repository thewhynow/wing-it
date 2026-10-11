#include "title.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <imgui.h>
#include <raylib.h>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "geometry.hpp"

namespace {

constexpr int    kSprite  = 160;     // sprite edge in px
constexpr int    kFrames  = 24;      // turntable frames
constexpr int    kCols    = 6;       // sheet layout
constexpr int    kRows    = kFrames / kCols;
constexpr int    kSS      = 2;       // supersampling factor
constexpr size_t kMaxTris = 24000;   // decimate bigger meshes

struct Floater {
    int sheet; float depth, x, y, speed, bob, bobSpeed, spin, phase, tilt;
};
struct Streak { float x, y, len, speed, alpha, thick; };

std::vector<Texture2D> gSheets;
std::vector<Floater>   gFloaters;
std::vector<Streak>    gStreaks;
bool gInited = false;

float frand(unsigned &s) { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0f; }

// software-rasterize kFrames yaw turntable frames of `mesh` into `sheet` (RGBA, kCols*kSprite wide)
void renderSheet(const TriMesh &mesh, Color base, Color rim, std::vector<Color> &sheet) {
    const int R = kSprite * kSS;
    const int W = kCols * kSprite;
    const size_t triCount = mesh.indices.size() / 3;
    const size_t stride = std::max<size_t>(1, (triCount + kMaxTris - 1) / kMaxTris);

    glm::vec3 lo(1e30f), hi(-1e30f);
    for (auto &v : mesh.vertices) lo = glm::min(lo, v), hi = glm::max(hi, v);
    const glm::vec3 c = (lo + hi) * 0.5f;
    float rad = 0;
    for (auto &v : mesh.vertices) rad = std::max(rad, glm::length(v - c));
    if (rad <= 0) return;
    const float scale = R * 0.47f / rad;

    std::vector<glm::vec3> pv(mesh.vertices.size());
    std::vector<Color> buf(R * R);
    std::vector<float> zb(R * R);
    const glm::vec3 L = glm::normalize(glm::vec3(-0.5f, 0.7f, 0.8f));
    const glm::vec3 Hh = glm::normalize(L + glm::vec3(0, 0, 1));
    const float pitch = 0.3f, cp = std::cos(pitch), sp = std::sin(pitch);

    for (int f = 0; f < kFrames; ++f) {
        const float yaw = 6.2831853f * f / kFrames, cy = std::cos(yaw), sy = std::sin(yaw);
        std::fill(buf.begin(), buf.end(), Color{base.r, base.g, base.b, 0});
        std::fill(zb.begin(), zb.end(), -1e30f);
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            glm::vec3 p = mesh.vertices[i] - c;
            glm::vec3 a(p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy);   // yaw
            glm::vec3 b(a.x, a.y * cp - a.z * sp, a.y * sp + a.z * cp);    // pitch
            pv[i] = glm::vec3(R * 0.5f + b.x * scale, R * 0.5f - b.y * scale, b.z * scale);
        }
        for (size_t t = 0; t < triCount; t += stride) {
            const glm::vec3 &A = pv[mesh.indices[t * 3]], &B = pv[mesh.indices[t * 3 + 1]], &C = pv[mesh.indices[t * 3 + 2]];
            // view-space normal (screen y is flipped); sign is arbitrary so face the camera
            glm::vec3 e1(B.x - A.x, A.y - B.y, B.z - A.z), e2(C.x - A.x, A.y - C.y, C.z - A.z);
            glm::vec3 n = glm::cross(e1, e2);
            float nl = glm::length(n);
            if (nl < 1e-6f) continue;
            n /= nl;
            if (n.z < 0) n = -n;
            float diff = std::max(0.0f, glm::dot(n, L));
            float rimk = std::pow(1.0f - n.z, 2.0f) * 0.55f;
            float spec = std::pow(std::max(0.0f, glm::dot(n, Hh)), 24.0f) * 0.35f;
            float k = 0.22f + 0.85f * diff;
            auto ch = [&](uint8_t bc, uint8_t rc) {
                return (uint8_t)std::min(255.0f, bc * k + rc * rimk + 255.0f * spec);
            };
            Color col{ch(base.r, rim.r), ch(base.g, rim.g), ch(base.b, rim.b), 255};

            int x0 = std::max(0, (int)std::floor(std::min({A.x, B.x, C.x})));
            int x1 = std::min(R - 1, (int)std::ceil(std::max({A.x, B.x, C.x})));
            int y0 = std::max(0, (int)std::floor(std::min({A.y, B.y, C.y})));
            int y1 = std::min(R - 1, (int)std::ceil(std::max({A.y, B.y, C.y})));
            float den = (B.y - C.y) * (A.x - C.x) + (C.x - B.x) * (A.y - C.y);
            if (std::fabs(den) < 1e-8f) continue;
            float inv = 1.0f / den;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    float px = x + 0.5f, py = y + 0.5f;
                    float w0 = ((B.y - C.y) * (px - C.x) + (C.x - B.x) * (py - C.y)) * inv;
                    float w1 = ((C.y - A.y) * (px - C.x) + (A.x - C.x) * (py - C.y)) * inv;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float z = w0 * A.z + w1 * B.z + w2 * C.z;
                    int idx = y * R + x;
                    if (z > zb[idx]) { zb[idx] = z; buf[idx] = col; }
                }
        }
        // box downsample into the sheet
        const int ox = (f % kCols) * kSprite, oy = (f / kCols) * kSprite;
        for (int y = 0; y < kSprite; ++y)
            for (int x = 0; x < kSprite; ++x) {
                int r = 0, g = 0, b = 0, cnt = 0;
                for (int j = 0; j < kSS; ++j)
                    for (int i = 0; i < kSS; ++i) {
                        const Color &p = buf[(y * kSS + j) * R + x * kSS + i];
                        if (p.a) r += p.r, g += p.g, b += p.b, ++cnt;
                    }
                Color o{base.r, base.g, base.b, 0};
                if (cnt) o = {(uint8_t)(r / cnt), (uint8_t)(g / cnt), (uint8_t)(b / cnt),
                              (uint8_t)(255 * cnt / (kSS * kSS))};
                sheet[(oy + y) * W + ox + x] = o;
            }
    }
}

void addSheet(const TriMesh &mesh, Color base, Color rim) {
    if (mesh.vertices.empty() || mesh.indices.size() < 3) return;
    std::vector<Color> pix(kCols * kSprite * kRows * kSprite);
    renderSheet(mesh, base, rim, pix);
    Image img{pix.data(), kCols * kSprite, kRows * kSprite, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D tex = LoadTextureFromImage(img); // uploads a GPU copy; CPU pixels freed on return
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(tex, TEXTURE_WRAP_CLAMP);
    gSheets.push_back(tex);
}

ImU32 hsv(float h, float s, float v, float a) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h - std::floor(h), s, v, r, g, b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), (int)(a * 255));
}

} // namespace

void titleInit() {
    if (gInited) return;
    gInited = true;

    const Color palette[8][2] = {
        {{90, 200, 255, 255}, {120, 255, 240, 255}},
        {{255, 150, 90, 255}, {255, 230, 140, 255}},
        {{170, 130, 255, 255}, {255, 140, 220, 255}},
        {{110, 230, 160, 255}, {200, 255, 200, 255}},
        {{255, 110, 150, 255}, {255, 190, 120, 255}},
        {{140, 170, 255, 255}, {150, 255, 255, 255}},
        {{240, 220, 120, 255}, {255, 255, 220, 255}},
        {{120, 220, 220, 255}, {180, 200, 255, 255}},
    };
    int pi = 0;

    std::vector<std::pair<std::string, uintmax_t>> files;
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator("./assets/models", ec)) {
        if (e.path().extension() != ".obj") continue;
        uintmax_t sz = e.file_size(ec);
        bool dup = false;
        for (auto &s : files) dup |= (s.second == sz);   // skips the duplicated bunny
        if (!dup) files.emplace_back(e.path().string(), sz);
    }
    std::sort(files.begin(), files.end());
    for (auto &s : files) {
        TriMesh m;
        if (!loadMesh(s.first, m)) continue;             // e.g. over 65535 verts
        const Color *p = palette[pi++ % 8];
        addSheet(m, p[0], p[1]);
    }
    // (makeSphere/makeNacaWing/makeCar are declared but not implemented yet - add them here once they exist)
    if (gSheets.empty()) return;

    unsigned seed = 1234567u;
    for (int i = 0; i < 16; ++i) {
        Floater f;
        f.sheet = i % (int)gSheets.size();
        f.depth = 0.25f + 0.75f * frand(seed);
        f.x = frand(seed);
        f.y = 0.08f + 0.84f * frand(seed);
        f.speed = 0.012f + 0.035f * f.depth;
        f.bob = 6 + 20 * frand(seed);
        f.bobSpeed = 0.4f + frand(seed);
        f.spin = (frand(seed) < 0.5f ? -1.0f : 1.0f) * (3.0f + 5.0f * frand(seed));
        f.phase = frand(seed) * kFrames;
        f.tilt = (frand(seed) - 0.5f) * 30.0f;
        gFloaters.push_back(f);
    }
    std::sort(gFloaters.begin(), gFloaters.end(), [](auto &a, auto &b) { return a.depth < b.depth; });
    for (int i = 0; i < 45; ++i) {
        Streak s;
        s.x = frand(seed); s.y = frand(seed);
        s.len = 80 + 220 * frand(seed);
        s.speed = 0.08f + 0.18f * frand(seed);
        s.alpha = 0.05f + 0.12f * frand(seed);
        s.thick = 1.0f + 1.5f * frand(seed);
        gStreaks.push_back(s);
    }
}

void titleShutdown() {
    for (auto &t : gSheets) UnloadTexture(t);
    gSheets.clear(); gFloaters.clear(); gStreaks.clear();
    gInited = false;
}

void titleDraw() {
    const float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    const float t = (float)GetTime();

    DrawRectangleGradientV(0, 0, (int)W, (int)H, Color{6, 10, 24, 255}, Color{22, 12, 44, 255});
    DrawCircleGradient((int)(W * (0.25f + 0.05f * std::sin(t * 0.2f))), (int)(H * 0.3f), W * 0.45f, Color{40, 90, 160, 55}, Color{0, 0, 0, 0});
    DrawCircleGradient((int)(W * (0.78f + 0.05f * std::cos(t * 0.17f))), (int)(H * 0.75f), W * 0.5f, Color{140, 50, 140, 45}, Color{0, 0, 0, 0});

    // wind streaks, blowing left to right
    for (auto &s : gStreaks) {
        float x = std::fmod(s.x + t * s.speed, 1.3f) - 0.15f;
        float px = x * W, py = s.y * H + std::sin(t * 0.6f + s.y * 9.0f) * 8.0f;
        DrawRectangleGradientH((int)(px - s.len), (int)py, (int)s.len, (int)s.thick,
                               Color{150, 210, 255, 0}, Color{150, 210, 255, (unsigned char)(s.alpha * 255)});
    }

    // floating models: far ones first (gFloaters is sorted by depth)
    const float tsz = (float)kSprite;
    for (auto &f : gFloaters) {
        float size = tsz * (0.9f + 1.5f * f.depth);
        float x = std::fmod(f.x + t * f.speed, 1.0f);
        float cx = -size + x * (W + 2 * size);
        float cy = f.y * H + std::sin(t * f.bobSpeed + f.phase) * f.bob * f.depth;
        int frame = ((int)std::floor(f.phase + t * f.spin) % kFrames + kFrames) % kFrames;
        Rectangle src{(float)(frame % kCols) * tsz, (float)(frame / kCols) * tsz, tsz, tsz};
        Rectangle dst{cx, cy, size, size};
        unsigned char a = (unsigned char)(255 * (0.25f + 0.5f * f.depth));
        DrawTexturePro(gSheets[f.sheet], src, dst, Vector2{size / 2, size / 2},
                       f.tilt + std::sin(t * 0.3f + f.phase) * 6.0f, Color{255, 255, 255, a});
    }
    DrawRectangleGradientV(0, 0, (int)W, (int)(H * 0.25f), Color{0, 0, 0, 120}, Color{0, 0, 0, 0});
    DrawRectangleGradientV(0, (int)(H * 0.75f), (int)W, (int)(H * 0.25f + 1), Color{0, 0, 0, 0}, Color{0, 0, 0, 140});

    // logo, drawn per letter with glow (ImGui draw list so any font size works)
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    ImFont *font = ImGui::GetFont();
    const char *title = "wing-it";
    const int n = 7;
    const float size = std::clamp(W * 0.17f, 60.0f, 170.0f);
    float adv[n], total = 0;
    for (int i = 0; i < n; ++i) {
        adv[i] = font->CalcTextSizeA(size, FLT_MAX, 0.0f, title + i, title + i + 1).x;
        total += adv[i];
    }
    float x = (W - total) * 0.5f;
    const float baseY = H * 0.14f;
    for (int i = 0; i < n; ++i) {
        float y = baseY + std::sin(t * 1.6f + i * 0.6f) * 7.0f;
        float hue = 0.52f + 0.22f * (float)i / (n - 1) + 0.04f * std::sin(t * 0.7f);
        for (int ring = 1; ring <= 3; ++ring) {
            float r = ring * 3.5f;
            ImU32 gc = hsv(hue, 0.7f, 1.0f, 0.10f / ring);
            for (int k = 0; k < 8; ++k) {
                float ang = k * 0.785398f;
                dl->AddText(font, size, ImVec2(x + std::cos(ang) * r, y + std::sin(ang) * r), gc, title + i, title + i + 1);
            }
        }
        dl->AddText(font, size, ImVec2(x + 3, y + 5), IM_COL32(0, 0, 0, 120), title + i, title + i + 1);
        dl->AddText(font, size, ImVec2(x, y), hsv(hue, 0.45f, 1.0f, 1.0f), title + i, title + i + 1);
        x += adv[i];
    }
    const char *sub = "a pocket wind tunnel";
    const float ss = std::clamp(W * 0.028f, 16.0f, 28.0f);
    ImVec2 sz = font->CalcTextSizeA(ss, FLT_MAX, 0.0f, sub);
    dl->AddText(font, ss, ImVec2((W - sz.x) * 0.5f, baseY + size * 1.12f),
                IM_COL32(190, 215, 255, (int)(150 + 60 * std::sin(t * 1.2f))), sub);
}

void titleMenuBegin() {
    const float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.62f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(260, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 22.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.30f, 0.55f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 0.90f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.70f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.80f, 1.00f, 0.6f));
    ImGui::Begin("TitleMenu", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
}

bool titleMenuButton(const char *label) {
    return ImGui::Button(label, ImVec2(-FLT_MIN, 48));
}

void titleMenuEnd() {
    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
}
