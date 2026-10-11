#include "style.hpp"

#include <filesystem>
#include <implot.h>

ImFont *gFontBody = nullptr, *gFontLight = nullptr, *gFontMedium = nullptr, *gFontMono = nullptr;

namespace {

// a missing file asserts inside ImGui, so check first. faceNo picks the face inside a .ttc collection
ImFont *tryFont(const char *path, float size, int faceNo = 0) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return nullptr;
    ImFontConfig cfg;
    cfg.FontNo = faceNo;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size, &cfg);
}

ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

} // namespace

void loadProFonts() {
    ImGuiIO &io = ImGui::GetIO();
    const char *ttc = "assets/fonts/HelveticaNeue.ttc", *ttf = "assets/fonts/HelveticaNeue-01.ttf";
    // HelveticaNeue.ttc faces: 0 Regular, 7 Light, 10 Medium
    gFontBody = tryFont(ttc, 18.0f, 0);
    if (!gFontBody) gFontBody = tryFont(ttf, 18.0f);
    gFontLight = tryFont(ttc, 54.0f, 7);
    gFontMedium = tryFont(ttc, 16.0f, 10);
    gFontMono = tryFont("/System/Library/Fonts/Menlo.ttc", 13.0f, 0);
    if (!gFontMono) gFontMono = tryFont("/System/Library/Fonts/SFNSMono.ttf", 13.0f);

    if (!gFontBody) gFontBody = io.FontDefault ? io.FontDefault : ImGui::GetIO().Fonts->Fonts[0];
    if (!gFontLight) gFontLight = gFontBody;
    if (!gFontMedium) gFontMedium = gFontBody;
    if (!gFontMono) gFontMono = gFontBody;
    io.FontDefault = gFontBody;
}

void applyProStyle() {
    ImGuiStyle &s = ImGui::GetStyle();
    s.FontSizeBase = 18.0f;

    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(8, 5);
    s.CellPadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(7, 4);
    s.IndentSpacing = 16;
    s.ScrollbarSize = 10;
    s.GrabMinSize = 9;

    s.WindowBorderSize = 1;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 1;
    s.TabBorderSize = 0;
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextPadding = ImVec2(0, 4);

    s.WindowRounding = 3;
    s.ChildRounding = 2;
    s.FrameRounding = 2;
    s.PopupRounding = 2;
    s.ScrollbarRounding = 2;
    s.GrabRounding = 2;
    s.TabRounding = 2;

    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.ButtonTextAlign = ImVec2(0.5f, 0.5f);

    const ImVec4 bg = rgb(18, 21, 26, 0.97f), panel = rgb(24, 28, 34), frame = rgb(28, 33, 40),
                 frameHi = rgb(35, 41, 49), frameAct = rgb(41, 48, 58), border = rgb(46, 53, 63),
                 text = rgb(214, 220, 227), dim = rgb(120, 130, 142), accent = rgb(86, 156, 214),
                 accentHi = rgb(122, 182, 232);
    auto a = [](ImVec4 c, float alpha) { c.w = alpha; return c; };

    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = dim;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(22, 26, 32, 0.98f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frameHi;
    c[ImGuiCol_FrameBgActive] = frameAct;
    c[ImGuiCol_TitleBg] = rgb(15, 18, 22);
    c[ImGuiCol_TitleBgActive] = rgb(15, 18, 22);
    c[ImGuiCol_TitleBgCollapsed] = rgb(15, 18, 22);
    c[ImGuiCol_MenuBarBg] = panel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = border;
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(62, 71, 83);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(78, 88, 102);
    c[ImGuiCol_CheckMark] = accentHi;
    c[ImGuiCol_CheckboxSelectedBg] = frame;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHi;
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = frameAct;
    c[ImGuiCol_ButtonActive] = a(accent, 0.55f);
    c[ImGuiCol_Header] = a(accent, 0.22f);
    c[ImGuiCol_HeaderHovered] = a(accent, 0.14f);
    c[ImGuiCol_HeaderActive] = a(accent, 0.32f);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = a(accent, 0.6f);
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = a(accent, 0.4f);
    c[ImGuiCol_ResizeGripActive] = a(accent, 0.7f);
    c[ImGuiCol_InputTextCursor] = text;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = frameAct;
    c[ImGuiCol_TabSelected] = frame;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = panel;
    c[ImGuiCol_TabDimmedSelected] = frame;
    c[ImGuiCol_TabDimmedSelectedOverline] = a(accent, 0.5f);
    c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_PlotLinesHovered] = accentHi;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = accentHi;
    c[ImGuiCol_TableHeaderBg] = panel;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = rgb(34, 40, 48);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_TextLink] = accentHi;
    c[ImGuiCol_TextSelectedBg] = a(accent, 0.35f);
    c[ImGuiCol_DragDropTarget] = accentHi;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_NavWindowingHighlight] = a(text, 0.6f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);

    // ImPlot: transparent canvas framed by a hairline, accent-led qualitative colormap
    ImPlotStyle &ps = ImPlot::GetStyle();
    ps.PlotBorderSize = 1;
    ps.MinorAlpha = 0.5f;
    ps.PlotPadding = ImVec2(6, 6);
    ps.LabelPadding = ImVec2(4, 3);
    ps.Colors[ImPlotCol_FrameBg] = ImVec4(0, 0, 0, 0);
    ps.Colors[ImPlotCol_PlotBg] = rgb(14, 17, 21, 0.9f);
    ps.Colors[ImPlotCol_PlotBorder] = border;
    ps.Colors[ImPlotCol_LegendBg] = rgb(22, 26, 32, 0.95f);
    ps.Colors[ImPlotCol_LegendBorder] = border;
    ps.Colors[ImPlotCol_LegendText] = text;
    ps.Colors[ImPlotCol_TitleText] = text;
    ps.Colors[ImPlotCol_InlayText] = text;
    ps.Colors[ImPlotCol_AxisText] = dim;
    ps.Colors[ImPlotCol_AxisGrid] = a(text, 0.07f);
    ps.Colors[ImPlotCol_AxisTick] = a(text, 0.15f);
    ps.Colors[ImPlotCol_Crosshairs] = a(text, 0.5f);
    static const ImVec4 series[] = {accent, rgb(224, 160, 82), rgb(132, 190, 140), rgb(196, 120, 120),
                                    rgb(160, 140, 200), rgb(120, 190, 196)};
    ps.Colormap = ImPlot::AddColormap("Graphite", series, (int)(sizeof series / sizeof series[0]));
}
