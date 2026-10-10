//
// Created by Adithiya Venkatakrishnan on 10/10/2026.
//

#include <imgui.h>
#include <rlImGui.h>
#include <thread>

#include "raylib.h"
#include "ui.hpp"
#include "GLFW/glfw3.h"

void time_thread(AppState& appState, std::atomic<bool>& running) {
    auto lastTick = std::chrono::steady_clock::now();

    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastTick);

        if (elapsed.count() >= 1) {
            appState.currentEpoch += elapsed.count();
            lastTick = now;
        }
    }
}

void key_loop(AppState& appState, UiState& uiState) {
    if (IsKeyPressed(KEY_A)) {
        appState.shouldQuit = true;
        return;
    }
    if (IsKeyDown(KEY_C)) {
        uiState.countUp++;
    }
}

void ui_loop(AppState& appState, UiState& uiState) {
    appState.frameRate = GetFPS();


    rlImGuiBegin();

    // --- 1. Frame Counter (Top-Left Corner of Screen) ---
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::Begin("HUD", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoInputs);

    ImGui::Text("Frames: %lld", appState.frameCount++);
    ImGui::Text("FPS: %lld", appState.frameRate.load());
    ImGui::End();


    // --- 2. Centered Title Window ---
    ImVec2 screenCenter = ImVec2(GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f);

    // ImVec2(0.5f, 0.5f) pivot centers the window right on screenCenter
    ImGui::SetNextWindowPos(screenCenter, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGui::Begin("TitleWindow", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoBackground);

    ImGui::SetWindowFontScale(3.0f); // Scaled title text
    ImGui::Text("Wing It!");
    ImGui::SetWindowFontScale(1.0f); // Reset font scale for subsequent text
    ImGui::Text("%d", uiState.countUp);

    ImGui::End();

    rlImGuiEnd();

}
