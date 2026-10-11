#pragma once
// Person 4
#include "common.hpp"
#include <atomic>

enum AppScreen {
    APP_SCREEN_WELCOME,
    APP_SCREEN_SELECT_MODEL,
    APP_SCREEN_UI,
};

struct AppState {
    AppScreen screen = APP_SCREEN_WELCOME;
    std::atomic<long long int> frameCount{0};
    std::atomic<long long int> frameRate{0};
    struct {
        int width = 1000, height = 800;
    } window;
    std::atomic<uint64_t> currentEpoch{0};
    bool shouldQuit;
};

struct UiState {
    SimParams params;
    int model = 0;
    int countUp = 0;
    int resolution = 1;
    float slicePos = 0.5f;
    bool paused = false, reset = false;
    glm::vec3 rotation{0.0f}; // model rotation in degrees about x, y, z
    bool rotationChanged = false; // set when a slider is let go, main re-voxelizes
    bool showColors = true, showLines = true;
};

// TODO: create functions to initialise UI ticks
// void uiDraw(UiState& ui, const Forces& forces, float stepsPerSec);
// void simStart(const Grid& grid, const SimParams& params);
// void simStop();
// bool simLatest(FlowField& field, Forces& forces);

void ui_loop(AppState& appState, UiState& uiState);
void time_thread(AppState& appState, std::atomic<bool>& running);
void key_loop(AppState& appState, UiState& uiState);