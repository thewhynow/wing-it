#include <algorithm>
#include <filesystem>
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

    for (const auto &entry : std::filesystem::directory_iterator("./assets/models"))
        if (entry.path().extension() == ".obj")
            uiState->models.push_back(entry.path().filename().string());
    std::sort(uiState->models.begin(), uiState->models.end());

    TriMesh rawMesh;

    // rotate the untouched mesh by the sliders, then fit, voxelize and restart the flow around it
    auto buildModel = [&] {
        TriMesh mesh = rawMesh;
        glm::mat4 r(1.0f);
        r = glm::rotate(r, glm::radians(uiState->rotation.x), {1, 0, 0});
        r = glm::rotate(r, glm::radians(uiState->rotation.y), {0, 1, 0});
        r = glm::rotate(r, glm::radians(uiState->rotation.z), {0, 0, 1});
        for (auto &v : mesh.vertices) v = glm::vec3(r * glm::vec4(v, 1.0f));
        for (auto &n : mesh.normals) n = glm::vec3(r * glm::vec4(n, 0.0f));

        Grid &g = cameraState->grid;
        g.nx = 2 * uiState->gridSize, g.ny = g.nz = uiState->gridSize;
        fitToGrid(mesh, g, uiState->params.aoaDeg, false);
        voxelize(mesh, cameraState->grid);
        solverReset(cameraState->grid, uiState->params); // voxel_loop reads the flow, so it needs cells
        renderSetMesh(mesh);
    };

    // solver runs flat out on its own thread, the render thread just snapshots it
    std::atomic<bool> solverRunning{false};
    std::thread solverThread;
    auto startSolver = [&] {
        solverRunning = true;
        solverThread = std::thread([&] {
            while (solverRunning)
                solverStep(cameraState->grid, uiState->params);
        });
    };
    auto stopSolver = [&] {
        solverRunning = false;
        if (solverThread.joinable())
            solverThread.join();
    };

    while (!WindowShouldClose() && !appState->shouldQuit) {
        BeginDrawing();
        ClearBackground(WHITE);
        key_loop(*appState, *uiState);
        if (appState->screen == APP_SCREEN_UI) {
            camera_loop(*appState, *cameraState);
            voxel_loop(*appState, *cameraState, *uiState);
        }
        ui_loop(*appState, *uiState);
        EndDrawing();

        // no point simulating behind the menus
        if (appState->screen != APP_SCREEN_UI && solverRunning)
            stopSolver();

        if (uiState->modelChosen) {
            uiState->modelChosen = false;
            const std::string &name = uiState->models[uiState->model];
            if (loadMesh("./assets/models/" + name, rawMesh)) {
                uiState->modelError.clear();
                uiState->rotation = glm::vec3(0.0f);
                uiState->params.aoaDeg = 0.0f;
                buildModel();
                startSolver();
                appState->screen = APP_SCREEN_UI;
            } else {
                uiState->modelError = "couldn't load " + name + " (over 65535 verts?)";
            }
        }

        // the solver reads the grid every step, so it has to be stopped while the model changes
        if (uiState->modelChanged) {
            uiState->modelChanged = false;
            stopSolver();
            buildModel();
            startSolver();
        }
    }

    threadRunning = false;
    timerThread.join();
    stopSolver();

    ImPlot::DestroyContext();
    rlImGuiShutdown();
    delete appState, delete uiState;

    CloseWindow();
}

