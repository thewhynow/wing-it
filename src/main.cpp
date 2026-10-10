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
    const auto cameraState = new Render3DState;

    InitWindow(appState->window.width, appState->window.height, "kill me :)");
    rlImGuiSetup(true);

    std::atomic<bool> threadRunning{true};
    std::thread timerThread(time_thread, std::ref(*appState), std::ref(threadRunning));

    ImGuiIO& io = ImGui::GetIO();
    SetExitKey(KEY_NULL);

    bodyFont = io.Fonts->AddFontFromFileTTF("assets/fonts/HelveticaNeue.ttc", 18.0f);
    if (bodyFont) {
        io.FontDefault = bodyFont;
    }

    ImPlot::CreateContext();
    printf("got here!");
    // we need to start by creating a vertex at the middle of every element
    std::vector<Vector2> morecoords;
    morecoords.reserve(cameraState->grid.nx * cameraState->grid.ny);
    for (int x = 0; x < cameraState->grid.nx; x++) {
        for (int y = 0; y < cameraState->grid.ny; y++) {
            morecoords.push_back(Vector2(x, y));
        }
    }
    cameraState->coords.push_back(morecoords);
    printf("got here!");

    while (!WindowShouldClose() && !appState->shouldQuit) {
        BeginDrawing();
        ClearBackground(BLACK);
        key_loop(*appState, *uiState);
        camera_loop(*appState, *cameraState);
        voxel_loop(*appState, *cameraState);
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

