#pragma once
// Person 4
#include "common.hpp"
#include <atomic>
#include <string>

enum AppScreen {
    APP_SCREEN_WELCOME,
    APP_SCREEN_SELECT_MODEL,
    APP_SCREEN_UI,
    // 2-player battle mode (src/ui/multiplayer.cpp)
    APP_SCREEN_MP_LOBBY,
    APP_SCREEN_MP_PICK,
    APP_SCREEN_MP_BATTLE,
    APP_SCREEN_MP_RESULT,
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
    bool modelChanged = false; // set when a slider is let go, main re-voxelizes
    int gridSize = 80; // tunnel is 2n long and n x n across
    bool showColors = true, showLines = true;
    std::vector<std::string> models; // .obj files in assets/models
    bool modelChosen = false; // set by Start, main loads models[model]
    std::string modelError;
};

// TODO: create functions to initialise UI ticks
// void uiDraw(UiState& ui, const Forces& forces, float stepsPerSec);
// void simStart(const Grid& grid, const SimParams& params);
// void simStop();
// bool simLatest(FlowField& field, Forces& forces);

void ui_loop(AppState& appState, UiState& uiState);
void time_thread(AppState& appState, std::atomic<bool>& running);
void key_loop(AppState& appState, UiState& uiState);