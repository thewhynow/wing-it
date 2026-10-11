#include <functional>
#include <implot.h>
#include <raylib.h>
#include <rlImGui.h>
#include <thread>
#include "render.hpp"
#include "ui.hpp"
#include "geometry.hpp"
#include "solver.hpp"

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
    printf("got here! grid.nx: %d, grid.ny: %d\n", cameraState->grid.nx, cameraState->grid.ny);

    TriMesh mesh;
    bool success = loadMesh("./assets/models/monkey.obj", mesh);
    if (success)
        printf("model %s loaded successfully!\n", "monkey.obj");

    fitToGrid(mesh, cameraState->grid, 0.0f, false);
    voxelize(mesh, cameraState->grid);
    solverReset(cameraState->grid, uiState->params); // voxel_loop reads the flow, so it needs cells
    renderSetMesh(mesh);

    // solver runs flat out on its own thread, the render thread just snapshots it
    std::atomic<bool> solverRunning{true};
    std::thread solverThread([&] {
        while (solverRunning)
            solverStep(cameraState->grid, uiState->params);
    });

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
    solverRunning = false;
    solverThread.join();

    ImPlot::DestroyContext();
    rlImGuiShutdown();
    delete appState, delete uiState;

    CloseWindow();
}

