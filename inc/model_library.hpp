#pragma once
// Model library UI shared by the singleplayer select screen (loops.cpp) and the multiplayer pick screen:
// grouped model list, "Add model file..." (native dialog, runs off the render thread), drag-and-drop,
// and the "Upload from phone" QR window. Selections are model IDs (paths, see models.hpp).
#include <string>
#include <imgui.h>

struct UiState;

// Call once per frame (before drawing any screen). Collects finished imports / phone uploads, rescans the
// library (ui.models is rebuilt and ui.model remapped by ID) and drains raylib's dropped-file queue;
// drops are imported only when acceptDrops is true, otherwise they're discarded.
void modelLibraryFrame(UiState& ui, bool acceptDrops);

// Rebuild ui.models from modelsList() after modelsRefresh(), keeping ui.model on the same ID.
void modelLibraryRefresh(UiState& ui);

int modelIndexOf(const UiState& ui, const std::string& id); // -1 if not found

// List box with "Sample models" / "My models" headers. Returns true when the user changed selectedId;
// *activated is set on double-click.
bool modelLibraryList(const char* strId, const UiState& ui, std::string& selectedId, ImVec2 size, bool* activated = nullptr);

// "Add model file..." + "Upload from phone" buttons, import status line, and the phone window when open.
// Returns true when it changed selectedId (a newly added model is auto-selected, or "Use this model" was
// clicked). Pass allowSelect = false to never touch the selection (e.g. while ready in multiplayer).
bool modelLibraryControls(const UiState& ui, std::string& selectedId, bool allowSelect = true);

// true while the phone window (a modal) is open - raw raylib hotkeys like Enter should be ignored then
bool modelLibraryModalOpen();
