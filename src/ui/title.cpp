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
    L.titleY = 72.0f;
    L.subY = L.titleY + 68.0f;
    L.view = {0.0f, 0.0f, L.W, L.H};
    L.readY = L.H - 24.0f;
    L.menuY = L.H - 152.0f; // room for two rows of buttons above the footer
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
bool gMenuNewRow = false;

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


    // simulation viewport
    const Box v = L.view;
    if (gRT.id && gVisitLoaded)
        DrawTexturePro(gRT.texture, {0, 0, (float)gRT.texture.width, -(float)gRT.texture.height}, {v.x, v.y, v.w, v.h},
                       {0, 0}, 0.0f, WHITE); // render textures are stored upside down
    else
        DrawRectangle((int)v.x, (int)v.y, (int)v.w, (int)v.h, toColor(pro::kPanel));

    const char *title = "Wing It";
    const float titleW = textW(gFontLight, 56.0f, title);
    dl->AddText(gFontLight, 56.0f, {std::round((W - titleW) * 0.5f), L.titleY}, pro::kText, title);

    // overlay labels
    const float ms = 12.0f;

    if (std::isfinite(gNextSwitch)) {
        const float p = std::clamp((float)((now - gShownAt) / (gNextSwitch - gShownAt)), 0.0f, 1.0f);
        dl->AddLine({v.x, v.y + v.h + 0.5f}, {v.x + v.w * p, v.y + v.h + 0.5f}, withAlpha(pro::kAccent, 0.7f));
    }

}

// --- menu: one centred row of flat buttons, the first one filled with the accent ---

void titleMenuBegin() {
    const Layout L = layout();
    gMenuIndex = 0;
    gMenuNewRow = false;
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
    if (!primary && !gMenuNewRow) ImGui::SameLine();
    gMenuNewRow = false;
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

void titleMenuNewRow() {
    ImGui::Dummy(ImVec2(0, 8));
    gMenuNewRow = true;
}

void titleMenuEnd() {
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(4);
}
