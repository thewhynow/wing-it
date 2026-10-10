#include <implot.h>
#include <raylib.h>
#include <rlImGui.h>
#include <thread>

#include "ui.hpp"

ImFont* bodyFont = nullptr;

int main() {
    const auto appState = new AppState;
    const auto uiState = new UiState;

    InitWindow(appState->window.width, appState->window.height, "Hello, World!");
    rlImGuiSetup(true);

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
