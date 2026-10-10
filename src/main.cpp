#include <functional>
#include <implot.h>
#include <raylib.h>
#include <rlImGui.h>
#include <thread>
#include "render.hpp"
#include "ui.hpp"

ImFont* bodyFont = nullptr;

int main() {
    const auto appState = new AppState;
    const auto uiState = new UiState;
    const auto cameraState = new CameraState;

    InitWindow(appState->window.width, appState->window.height, "Hello, World!");
    rlImGuiSetup(true);
    InitWindow(799, 450, "raylib [core] example - 3d mode");

    std::atomic<bool> threadRunning{true};
    std::thread timerThread(time_thread, std::ref(*appState), std::ref(threadRunning));

    ImGuiIO& io = ImGui::GetIO();
    SetExitKey(KEY_NULL);

    bodyFont = io.Fonts->AddFontFromFileTTF("assets/fonts/HelveticaNeue.ttc", 18.0f);
    if (bodyFont) {
        io.FontDefault = bodyFont;
    }

    ImPlot::CreateContext();

    while (!WindowShouldClose() && !appState->shouldQuit) {
        BeginDrawing();
        ClearBackground(BLACK);
        key_loop(*appState, *uiState);
        camera_loop(*appState, *cameraState);
        ui_loop(*appState, *uiState);
        EndDrawing();
    }

    threadRunning = false;
    timerThread.join();

    ImPlot::DestroyContext();
    rlImGuiShutdown();
    delete appState, delete uiState;

    CloseWindow();
}

