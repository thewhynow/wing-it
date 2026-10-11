#include "title.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include <imgui.h>
#include <raylib.h>

#include "geometry.hpp"
#include "models.hpp"
#include "render.hpp"
#include "solver.hpp"
#include "style.hpp"
#include "ui.hpp"

namespace {

constexpr int   kSS = 2;          // offscreen supersampling, smooths the 1px lines and ribbon edges
constexpr float kFade = 0.45f;    // seconds the model name takes to fade in / out
constexpr float kOrbit = 0.11f;   // camera yaw speed, rad/s
constexpr float kMargin = 56.0f;

struct Entry {
    std::string path, file, name;
    bool loaded = false, bad = false;
    TriMesh mesh;
};

std::vector<Entry> gEntries;
int    gCurrent = -1;          // entry the sim is running, -1 before the first one
bool   gActive = false;        // on the welcome screen
bool   gVisitLoaded = false;   // a model has been built since the welcome screen was (re)entered
double gShownAt = 0, gNextSwitch = 0;
float  gYaw = 0.9f;
float  gNameW = -1;            // eased width of the name, so the line re-centres smoothly
int    gGrid[3] = {0, 0, 0};

RenderTexture2D gRT{};
UiState *gUi = nullptr;        // display flags for voxel_loop (colours + streamlines on), independent of the user's

// "stanford-bunny (1)" -> "Stanford Bunny"
std::string cleanName(std::string s) {
    if (auto p = s.rfind('('); p != std::string::npos && s.back() == ')') {
        bool digits = p + 2 < s.size();
        for (size_t i = p + 1; i + 1 < s.size(); ++i) digits &= std::isdigit((unsigned char)s[i]) != 0;
        if (digits) s.erase(p);
    }
    std::string out;
    bool startOfWord = true;
    for (char ch : s) {
        if (ch == '-' || ch == '_' || ch == '.' || std::isspace((unsigned char)ch)) {
            startOfWord = true;
            continue;
        }
        if (startOfWord && !out.empty()) out += ' ';
        out += startOfWord ? (char)std::toupper((unsigned char)ch) : ch;
        startOfWord = false;
    }
    return out;
}

struct Box { float x, y, w, h; };

// page layout, shared by the offscreen render (size) and the drawing
struct Layout {
    float W, H, titleY, subY;
    Box view;
    float readY, menuY, footY;
};

Layout layout() {
    Layout L;
    L.W = (float)GetScreenWidth(), L.H = (float)GetScreenHeight();
    L.titleY = 70.0f;
    L.subY = L.titleY + 68.0f;
    float top = L.subY + 50.0f, bottom = std::max(top + 120.0f, L.H - 170.0f);
    L.view = {kMargin, top, std::max(120.0f, L.W - 2 * kMargin), bottom - top};
    L.readY = bottom + 11.0f;
    L.menuY = bottom + 42.0f;
    L.footY = L.H - 44.0f;
    return L;
}

Color toColor(ImU32 c, float alpha = 1.0f) {
    return {(unsigned char)(c & 0xFF), (unsigned char)((c >> 8) & 0xFF), (unsigned char)((c >> 16) & 0xFF),
            (unsigned char)(((c >> 24) & 0xFF) * alpha)};
}
ImU32 withAlpha(ImU32 c, float a) {
    return (c & 0x00FFFFFFu) | ((ImU32)std::clamp(((c >> 24) & 0xFF) * a, 0.0f, 255.0f) << 24);
}

// same ramp as the pressure colours in render.cpp
ImU32 jet(float t) {
    auto ch = [&](float c) { return (int)(255 * std::clamp(1.5f - std::fabs(4.0f * t - c), 0.0f, 1.0f)); };
    return IM_COL32(ch(3), ch(2), ch(1), 255);
}

float textW(ImFont *f, float size, const char *s) { return f->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x; }

// L-shaped registration marks on the corners of a box
void cornerMarks(ImDrawList *dl, Box b, float len, ImU32 col) {
    const float x0 = b.x - 0.5f, y0 = b.y - 0.5f, x1 = b.x + b.w + 0.5f, y1 = b.y + b.h + 0.5f;
    dl->AddLine({x0, y0}, {x0 + len, y0}, col); dl->AddLine({x0, y0}, {x0, y0 + len}, col);
    dl->AddLine({x1, y0}, {x1 - len, y0}, col); dl->AddLine({x1, y0}, {x1, y0 + len}, col);
    dl->AddLine({x0, y1}, {x0 + len, y1}, col); dl->AddLine({x0, y1}, {x0, y1 - len}, col);
    dl->AddLine({x1, y1}, {x1 - len, y1}, col); dl->AddLine({x1, y1}, {x1, y1 - len}, col);
}

int gMenuIndex = 0;

} // namespace

void titleInit() {
    gEntries.clear();
    std::vector<std::pair<std::filesystem::path, uintmax_t>> files;
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(kSampleDir, ec)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".obj") files.emplace_back(e.path(), e.file_size(ec));
    }
    std::sort(files.begin(), files.end());
    for (auto &[path, size] : files) {
        std::string name = cleanName(path.stem().string());
        bool dup = name.empty();
        for (const Entry &e : gEntries)
            dup |= e.name == name || std::filesystem::file_size(e.path, ec) == size; // the duplicated bunny
        if (dup) continue;
        Entry e;
        e.path = path.generic_string();
        e.file = path.filename().string();
        e.name = name;
        gEntries.push_back(std::move(e));
    }
    if (!gUi) gUi = new UiState; // defaults: pressure colours and streamlines on
}

void titleShutdown() {
    if (gRT.id) UnloadRenderTexture(gRT);
    gRT = {};
    gEntries.clear();
    delete gUi;
    gUi = nullptr;
}

const TriMesh *titleSimNextModel() {
    const double now = GetTime();
    if (!gActive) { // (re)entered the welcome screen: build straight away
        gActive = true;
        gVisitLoaded = false;
        gNextSwitch = now;
    }
    if (now < gNextSwitch || gEntries.empty()) return nullptr;

    // fixed cadence; only re-anchor if we fell a whole period behind (e.g. a long stall)
    gNextSwitch = gNextSwitch + kTitleModelSeconds > now ? gNextSwitch + kTitleModelSeconds : now + kTitleModelSeconds;

    const int n = (int)gEntries.size();
    for (int k = 1; k <= n; ++k) {
        const int i = (gCurrent + k) % n;
        Entry &e = gEntries[i];
        if (e.bad) continue;
        if (!e.loaded) {
            e.loaded = true;
            e.bad = !loadMesh(e.path, e.mesh) || e.mesh.indices.empty(); // e.g. over 65535 verts
            if (e.bad) {
                TraceLog(LOG_WARNING, "title: skipping %s", e.path.c_str());
                continue;
            }
        }
        if (i == gCurrent && gVisitLoaded) return nullptr; // only one usable model, keep its flow running
        gCurrent = i;
        gVisitLoaded = true;
        gShownAt = now;
        return &e.mesh;
    }
    gNextSwitch = INFINITY; // nothing loadable
    return nullptr;
}

void titleSimLeave() {
    gActive = false;
    gVisitLoaded = false;
}

void titleSimRender(AppState &appState, Render3DState &cs) {
    const Layout L = layout();
    const int w = (int)L.view.w * kSS, h = (int)L.view.h * kSS;
    if (gRT.id == 0 || gRT.texture.width != w || gRT.texture.height != h) {
        if (gRT.id) UnloadRenderTexture(gRT);
        gRT = LoadRenderTexture(w, h);
        SetTextureFilter(gRT.texture, TEXTURE_FILTER_BILINEAR);
    }

    gYaw += kOrbit * GetFrameTime();
    const Grid &g = cs.grid;
    gGrid[0] = g.nx, gGrid[1] = g.ny, gGrid[2] = g.nz;

    BeginTextureMode(gRT);
    ClearBackground(toColor(pro::kPanel));
    if (gVisitLoaded) {
        // the scene is centred on the tunnel; aim a little upstream, toward the model (it sits at nx/4)
        const Vector3 target = {-0.10f * g.nx, -0.10f * g.ny, 0.0f};
        const float dist = 1.40f * g.nx, pitch = 0.42f;
        const Camera saved = cs.camera;
        cs.camera.target = target;
        cs.camera.up = {0, 1, 0};
        cs.camera.fovy = 40.0f;
        cs.camera.projection = CAMERA_PERSPECTIVE;
        cs.camera.position = {target.x + dist * std::cos(pitch) * std::sin(gYaw), target.y + dist * std::sin(pitch),
                              target.z + dist * std::cos(pitch) * std::cos(gYaw)};
        voxel_loop(appState, cs, *gUi);
        cs.camera = saved; // the user's orbit camera is untouched
    }
    EndTextureMode();
}

void titleDraw() {
    const Layout L = layout();
    const float W = L.W, H = L.H;
    const double now = GetTime();
    ImDrawList *dl = ImGui::GetBackgroundDrawList();

    // background + faint drafting grid
    DrawRectangle(0, 0, (int)W, (int)H, toColor(pro::kBg));
    for (int x = 0, i = 0; x < (int)W; x += 24, ++i)
        DrawRectangle(x, 0, 1, (int)H, toColor(pro::kGridLine, i % 4 ? 0.55f : 1.0f));
    for (int y = 0, i = 0; y < (int)H; y += 24, ++i)
        DrawRectangle(0, y, (int)W, 1, toColor(pro::kGridLine, i % 4 ? 0.55f : 1.0f));

    // header strip
    dl->AddText(gFontMono, 12.0f, {kMargin, 19}, pro::kTextDim, "WING-IT");
    dl->AddText(gFontMono, 12.0f, {kMargin + textW(gFontMono, 12.0f, "WING-IT") + 14, 19}, pro::kTextFaint,
                "VIRTUAL WIND TUNNEL");
    {
        const char *r = "REAL-TIME CFD";
        dl->AddText(gFontMono, 12.0f, {W - kMargin - textW(gFontMono, 12.0f, r), 19}, pro::kTextFaint, r);
    }
    dl->AddLine({kMargin, 44.5f}, {W - kMargin, 44.5f}, pro::kHairline);

    // "Aerodynamics of a <Name>", the name fades out/in around each switch
    const Entry *cur = gCurrent >= 0 && gVisitLoaded ? &gEntries[gCurrent] : nullptr;
    const std::string name = cur ? cur->name : "";
    const bool vowel = !name.empty() && std::string("AEIOU").find(name[0]) != std::string::npos;
    const std::string prefix = vowel ? "Aerodynamics of an " : "Aerodynamics of a ";
    const float ts = 54.0f;
    const float nameTarget = name.empty() ? textW(gFontLight, ts, "Model") : textW(gFontLight, ts, name.c_str());
    if (gNameW < 0) gNameW = nameTarget;
    gNameW += (nameTarget - gNameW) * (1.0f - std::exp(-8.0f * GetFrameTime()));
    const float prefixW = textW(gFontLight, ts, prefix.c_str());
    const float tx = std::round((W - prefixW - gNameW) * 0.5f);
    dl->AddText(gFontLight, ts, {tx, L.titleY}, pro::kText, prefix.c_str());

    float nameA = 0.0f;
    if (cur) {
        const float in = std::clamp((float)(now - gShownAt) / kFade, 0.0f, 1.0f);
        const float out = std::isfinite(gNextSwitch) ? std::clamp((float)(gNextSwitch - now) / kFade, 0.0f, 1.0f) : 1.0f;
        nameA = std::min(in, out);
        nameA = nameA * nameA * (3 - 2 * nameA);
    }
    const float nx = tx + prefixW;
    if (cur) dl->AddText(gFontLight, ts, {nx, L.titleY - 4.0f * (1.0f - nameA)}, withAlpha(IM_COL32(236, 241, 246, 255), nameA), name.c_str());
    // the "blank": a hairline under the name slot
    const float lineY = std::round(L.titleY + ts * 1.08f) + 0.5f;
    dl->AddLine({nx, lineY}, {nx + gNameW, lineY}, pro::kHairline);
    dl->AddLine({nx, lineY}, {nx + gNameW * nameA, lineY}, withAlpha(pro::kAccent, 0.9f));

    {
        const char *sub = "Live D3Q19 lattice-Boltzmann flow, solved in real time";
        const float ss = 20.0f;
        dl->AddText(gFontBody, ss, {std::round((W - textW(gFontBody, ss, sub)) * 0.5f), L.subY}, pro::kTextDim, sub);
    }

    // simulation viewport
    const Box v = L.view;
    if (gRT.id && gVisitLoaded)
        DrawTexturePro(gRT.texture, {0, 0, (float)gRT.texture.width, -(float)gRT.texture.height}, {v.x, v.y, v.w, v.h},
                       {0, 0}, 0.0f, WHITE); // render textures are stored upside down
    else
        DrawRectangle((int)v.x, (int)v.y, (int)v.w, (int)v.h, toColor(pro::kPanel));
    dl->AddRect({v.x - 0.5f, v.y - 0.5f}, {v.x + v.w + 0.5f, v.y + v.h + 0.5f}, pro::kHairline);
    cornerMarks(dl, v, 9.0f, withAlpha(pro::kAccent, 0.8f));

    // overlay labels
    const float ms = 12.0f;
    dl->AddText(gFontMono, ms, {v.x + 12, v.y + 10}, pro::kTextDim, "FLOW  +X");
    dl->AddLine({v.x + 12 + textW(gFontMono, ms, "FLOW  +X") + 8, v.y + 17.5f},
                {v.x + 12 + textW(gFontMono, ms, "FLOW  +X") + 40, v.y + 17.5f}, pro::kTextDim);
    {
        const char *r = "PRESSURE COLOUR  /  STREAMLINES";
        dl->AddText(gFontMono, ms, {v.x + v.w - 12 - textW(gFontMono, ms, r), v.y + 10}, pro::kTextFaint, r);
    }
    // pressure scale, bottom right
    {
        const float bw = 120, bh = 5, bx = v.x + v.w - 12 - bw, by = v.y + v.h - 24;
        const int seg = 24;
        dl->AddRectFilled({bx - 56, by - 10}, {v.x + v.w - 3, v.y + v.h - 3}, withAlpha(pro::kPanel, 0.85f), 2.0f);
        for (int i = 0; i < seg; ++i)
            dl->AddRectFilledMultiColor({bx + bw * i / seg, by}, {bx + bw * (i + 1) / seg, by + bh},
                                        jet((float)i / seg), jet((float)(i + 1) / seg), jet((float)(i + 1) / seg), jet((float)i / seg));
        dl->AddRect({bx - 0.5f, by - 0.5f}, {bx + bw + 0.5f, by + bh + 0.5f}, withAlpha(IM_COL32(0, 0, 0, 255), 0.6f));
        dl->AddText(gFontMono, 11.0f, {bx - textW(gFontMono, 11.0f, "p  LOW") - 8, by - 4}, pro::kTextDim, "p  LOW");
        dl->AddText(gFontMono, 11.0f, {bx + bw - textW(gFontMono, 11.0f, "HIGH"), by + bh + 3}, pro::kTextFaint, "HIGH");
    }
    if (!gVisitLoaded) {
        const char *m = gEntries.empty() ? "NO SAMPLE MODELS FOUND" : "INITIALISING SOLVER";
        dl->AddText(gFontMono, ms, {std::round(v.x + (v.w - textW(gFontMono, ms, m)) * 0.5f), v.y + v.h * 0.5f - 6}, pro::kTextDim, m);
    }
    // time until the next model, as a hairline along the bottom edge
    if (cur && std::isfinite(gNextSwitch)) {
        const float p = std::clamp((float)((now - gShownAt) / (gNextSwitch - gShownAt)), 0.0f, 1.0f);
        dl->AddLine({v.x, v.y + v.h + 0.5f}, {v.x + v.w * p, v.y + v.h + 0.5f}, withAlpha(pro::kAccent, 0.7f));
    }

    // readouts under the viewport
    {
        char buf[160];
        float x = v.x;
        auto field = [&](const char *label, const char *value) {
            dl->AddText(gFontMono, ms, {x, L.readY}, pro::kTextFaint, label);
            x += textW(gFontMono, ms, label) + 8;
            dl->AddText(gFontMono, ms, {x, L.readY}, pro::kText, value);
            x += textW(gFontMono, ms, value) + 28;
        };
        field("MODEL", cur ? cur->file.c_str() : "--");
        snprintf(buf, sizeof buf, "%d x %d x %d", gGrid[0], gGrid[1], gGrid[2]);
        field("GRID", cur ? buf : "--");
        const float cd = solverForces({}, {}).cd;
        snprintf(buf, sizeof buf, "%.3f", cd);
        field("Cd", cur && now - gShownAt > 1.0 && std::isfinite(cd) ? buf : "--");

        int usable = 0, idx = 0;
        for (int i = 0; i < (int)gEntries.size(); ++i)
            if (!gEntries[i].bad) {
                ++usable;
                if (i == gCurrent) idx = usable;
            }
        snprintf(buf, sizeof buf, "%02d / %02d", cur ? idx : 0, usable);
        dl->AddText(gFontMono, ms, {v.x + v.w - textW(gFontMono, ms, buf), L.readY}, pro::kTextFaint, buf);
    }

    // footer
    dl->AddLine({kMargin, L.footY + 0.5f}, {W - kMargin, L.footY + 0.5f}, pro::kHairline);
    dl->AddText(gFontBody, 14.0f, {kMargin, L.footY + 13}, pro::kTextFaint, "D3Q19 Lattice Boltzmann  \xC2\xB7  wind tunnel");
    {
        const char *r = "PRESS ENTER TO BEGIN";
        dl->AddText(gFontMono, 12.0f, {W - kMargin - textW(gFontMono, 12.0f, r), L.footY + 15}, pro::kTextFaint, r);
    }
}

// --- menu: one centred row of flat buttons, the first one filled with the accent ---

void titleMenuBegin() {
    const Layout L = layout();
    gMenuIndex = 0;
    ImGui::SetNextWindowPos(ImVec2(L.W * 0.5f, L.menuY), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 0));
    ImGui::Begin("TitleMenu", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(gFontMedium, 14.0f);
}

bool titleMenuButton(const char *label) {
    // small caps look: upper-case the visible part, keep any "##id" suffix as is
    std::string vis(label);
    const size_t hash = vis.find("##");
    std::transform(vis.begin(), hash == std::string::npos ? vis.end() : vis.begin() + hash, vis.begin(), ::toupper);
    if (vis != label && hash == std::string::npos) vis += std::string("###") + label;

    const bool primary = gMenuIndex++ == 0;
    if (!primary) ImGui::SameLine();
    const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(pro::kAccent);
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x * 0.85f, accent.y * 0.85f, accent.z * 0.85f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x * 0.7f, accent.y * 0.7f, accent.z * 0.7f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x, accent.y, accent.z, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.98f, 1.0f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(withAlpha(pro::kPanel, 0.9f)));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.17f, 0.21f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x, accent.y, accent.z, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(pro::kHairline));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pro::kText));
    }
    const bool pressed = ImGui::Button(vis.c_str(), ImVec2(196, 40));
    ImGui::PopStyleColor(5);
    return pressed;
}

void titleMenuEnd() {
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(4);
}
