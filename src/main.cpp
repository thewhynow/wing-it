#include <implot.h>
#include <raylib.h>
#include <rlImGui.h>
#include <thread>
#include "vis.h"

#include "ui.hpp"

ImFont* bodyFont = nullptr;

int main() {

    
    const auto appState = new AppState;
    const auto uiState = new UiState;

    InitWindow(appState->window.width, appState->window.height, "Hello, World!");
    rlImGuiSetup(true);
    InitWindow(799, 450, "raylib [core] example - 3d mode");

    Camera3D camera = { 0 };
    camera.position = (Vector3){ 0.0f, 10.0f, 10.0f };
    camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };    
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };   
    camera.fovy = 45.0f;    
    camera.projection = CAMERA_PERSPECTIVE;

    SetTargetFPS(60);


    std::atomic<bool> threadRunning{true};
    std::thread timerThread(time_thread, std::ref(*appState), std::ref(threadRunning));
    std::thread keyThread(key_thread, std::ref(*appState), std::ref(*uiState), std::ref(threadRunning));

    ImGuiIO& io = ImGui::GetIO();
    SetExitKey(KEY_NULL);

    bodyFont = io.Fonts->AddFontFromFileTTF("assets/fonts/HelveticaNeue.ttc", 18.0f);
    if (bodyFont) {
        io.FontDefault = bodyFont;
    }

    ImPlot::CreateContext();

    while (!WindowShouldClose() && !appState->shouldQuit) {
        create3d(camera);
        render_loop(*appState, *uiState);
    }

    threadRunning = false;
    timerThread.join();
    keyThread.join();

    ImPlot::DestroyContext();
    rlImGuiShutdown();
    delete appState, delete uiState;

    CloseWindow();
}

