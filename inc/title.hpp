#pragma once
// Title screen: "Aerodynamics of a <model>" over a live, slowly orbiting simulation of the sample models.
// The flow is the real solver on main's solver thread; this module only decides which model to show,
// renders the scene into an offscreen target and draws the page around it.
#include "common.hpp"

struct Render3DState;
struct AppState;

constexpr int    kTitleGridSize     = 44;   // small tunnel (88 x 44 x 44) so the title sim stays cheap
constexpr double kTitleModelSeconds = 7.0;  // each model is shown for exactly this long

void titleInit();      // call once after InitWindow (scans the sample models)
void titleShutdown();  // call before CloseWindow

// --- live simulation, driven from main on the welcome screen ---
// Returns the next mesh (raw, as loaded) when it's time to switch models, else nullptr. main then stops the
// solver, rebuilds the tunnel around it at kTitleGridSize and restarts the flow. Unloadable files are skipped.
const TriMesh *titleSimNextModel();
void titleSimRender(AppState &appState, Render3DState &cameraState); // offscreen render of the current flow, call inside BeginDrawing
void titleSimLeave();                            // call every frame off the welcome screen; re-entry reloads

// --- drawing (inside the ImGui frame) ---
void titleDraw();      // page background, title, simulation viewport, readouts, footer

// Menu helpers: titleMenuBegin(); if (titleMenuButton("Enter")) ...; titleMenuButton("Multiplayer"); titleMenuEnd();
// Buttons are laid out in one centred row under the viewport; the first one is the primary action.
void titleMenuBegin();
bool titleMenuButton(const char *label);
void titleMenuEnd();
