#pragma once
// Global look: flat graphite palette, hairline borders, one steel-blue accent, Helvetica Neue.
#include <imgui.h>

// Faces loaded by loadProFonts(). Never null after loading (missing files fall back to the default font).
// With ImGui 1.92 any face can be drawn at any size: ImGui::PushFont(gFontLight, 54.0f) / drawList->AddText(font, size, ...)
extern ImFont *gFontBody;   // Helvetica Neue Regular, 18px, also io.FontDefault
extern ImFont *gFontLight;  // Helvetica Neue Light, for large titles
extern ImFont *gFontMedium; // Helvetica Neue Medium, for buttons / labels
extern ImFont *gFontMono;   // Menlo (or SF Mono), for small readouts / captions

namespace pro {
// palette shared by the ImGui style and custom-drawn screens
constexpr ImU32 kBg       = IM_COL32(13, 16, 20, 255);
constexpr ImU32 kPanel    = IM_COL32(20, 24, 30, 255);
constexpr ImU32 kHairline = IM_COL32(44, 51, 60, 255);
constexpr ImU32 kGridLine = IM_COL32(255, 255, 255, 7);
constexpr ImU32 kText     = IM_COL32(214, 220, 227, 255);
constexpr ImU32 kTextDim  = IM_COL32(128, 138, 150, 255);
constexpr ImU32 kTextFaint= IM_COL32(86, 95, 106, 255);
constexpr ImU32 kAccent   = IM_COL32(86, 156, 214, 255);
}

void loadProFonts();   // call once after rlImGuiSetup
void applyProStyle();  // call once after ImPlot::CreateContext
