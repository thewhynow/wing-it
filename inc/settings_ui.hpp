#pragma once
// Settings window (Hack Club AI API key + model). A modal that can be opened from any screen.
void settingsUiRequestOpen();
bool settingsUiModalOpen();   // true while the window is up - raw raylib hotkeys must be ignored then
void settingsUiDraw();        // call every frame, outside of any other ImGui window
