#include <functional>
#include <implot.h>
#include <raylib.h>
#include <rlImGui.h>
#include <thread>
#include <glm/gtc/matrix_transform.hpp>
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

    TriMesh rawMesh;
    bool success = loadMesh("./assets/models/monkey.obj", rawMesh);
    if (success)
        printf("model %s loaded successfully!\n", "monkey.obj");

    // rotate the untouched mesh by the sliders, then fit, voxelize and restart the flow around it
    auto buildModel = [&] {
        TriMesh mesh = rawMesh;
        glm::mat4 r(1.0f);
        r = glm::rotate(r, glm::radians(uiState->rotation.x), {1, 0, 0});
        r = glm::rotate(r, glm::radians(uiState->rotation.y), {0, 1, 0});
        r = glm::rotate(r, glm::radians(uiState->rotation.z), {0, 0, 1});
        for (auto &v : mesh.vertices) v = glm::vec3(r * glm::vec4(v, 1.0f));
        for (auto &n : mesh.normals) n = glm::vec3(r * glm::vec4(n, 0.0f));

        fitToGrid(mesh, cameraState->grid, 0.0f, false);
        voxelize(mesh, cameraState->grid);
        solverReset(cameraState->grid, uiState->params); // voxel_loop reads the flow, so it needs cells
        renderSetMesh(mesh);
    };
    buildModel();

    // solver runs flat out on its own thread, the render thread just snapshots it
    std::atomic<bool> solverRunning{true};
    std::thread solverThread;
    auto startSolver = [&] {
        solverRunning = true;
        solverThread = std::thread([&] {
            while (solverRunning)
                solverStep(cameraState->grid, uiState->params);
        });
    };
    startSolver();

    while (!WindowShouldClose() && !appState->shouldQuit) {
        BeginDrawing();
        ClearBackground(WHITE);
        key_loop(*appState, *uiState);
        camera_loop(*appState, *cameraState);
        voxel_loop(*appState, *cameraState, *uiState);
        ui_loop(*appState, *uiState);
        EndDrawing();

        // the solver reads the grid every step, so it has to be stopped while the model changes
        if (uiState->rotationChanged) {
            uiState->rotationChanged = false;
            solverRunning = false;
            solverThread.join();
            buildModel();
            startSolver();
        }
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

