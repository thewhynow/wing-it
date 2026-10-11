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


    ImVec2 screenCenter = ImVec2(GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f);

    if (appState.screen == APP_SCREEN_WELCOME) {
        // ImVec2(0.5f, 0.5f) pivot centers the window right on screenCenter
        ImGui::SetNextWindowPos(screenCenter, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin("TitleWindow", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoBackground);

        ImGui::SetWindowFontScale(3.0f); // Scaled title text
        ImGui::Text("Wing It!");
        ImGui::SetWindowFontScale(1.0f); // Reset font scale for subsequent text
        ImGui::TextDisabled("a pocket wind tunnel");
        ImGui::Spacing();
        if (ImGui::Button("Enter", ImVec2(-FLT_MIN, 0)) || IsKeyPressed(KEY_ENTER))
            appState.screen = APP_SCREEN_SELECT_MODEL;

        ImGui::End();
    } else if (appState.screen == APP_SCREEN_SELECT_MODEL) {
        ImGui::SetNextWindowPos(screenCenter, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin("Pick a model", nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_AlwaysAutoResize);

        if (ImGui::BeginListBox("##models", ImVec2(260, 0))) {
            for (int i = 0; i < (int)uiState.models.size(); ++i)
                if (ImGui::Selectable(uiState.models[i].c_str(), uiState.model == i, ImGuiSelectableFlags_AllowDoubleClick)
                    && (uiState.model = i, ImGui::IsMouseDoubleClicked(0)))
                    uiState.modelChosen = true;
            ImGui::EndListBox();
        }
        if (!uiState.modelError.empty())
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", uiState.modelError.c_str());
        if (ImGui::Button("Back"))
            appState.screen = APP_SCREEN_WELCOME;
        ImGui::SameLine();
        if (ImGui::Button("Start", ImVec2(-FLT_MIN, 0)) || IsKeyPressed(KEY_ENTER))
            uiState.modelChosen = !uiState.models.empty();

        ImGui::End();
    }

    if (appState.screen != APP_SCREEN_UI) {
        rlImGuiEnd();
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(GetScreenWidth() - 10.0f, 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    // only rebuild once the slider is let go, re-voxelizing restarts the flow
    const char *axes[] = {"Rotate X", "Rotate Y", "Rotate Z"};
    for (int i = 0; i < 3; ++i) {
        ImGui::SliderFloat(axes[i], &uiState.rotation[i], -180.0f, 180.0f, "%.0f deg");
        if (ImGui::IsItemDeactivatedAfterEdit())
            uiState.modelChanged = true;
    }
    ImGui::SliderFloat("AoA", &uiState.params.aoaDeg, -30.0f, 30.0f, "%.1f deg"); // pitch, positive is nose up
    if (ImGui::IsItemDeactivatedAfterEdit())
        uiState.modelChanged = true;
    ImGui::SliderInt("Grid size", &uiState.gridSize, 40, 112, "%d cells");
    if (ImGui::IsItemDeactivatedAfterEdit())
        uiState.modelChanged = true;

    // the inlet reads this every step so it applies live. lattice units, keep it well under the sound speed (0.577)
    ImGui::SliderFloat("Wind speed", &uiState.params.inletU, 0.01f, 0.12f, "%.3f");
    // changing this live slams a pressure wave through the tunnel, so it restarts the flow at the new density
    ImGui::SliderFloat("Air density", &uiState.params.inletRho, 0.5f, 2.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit())
        uiState.modelChanged = true;
    ImGui::Checkbox("Pressure colours", &uiState.showColors);
    ImGui::Checkbox("Streamlines", &uiState.showLines);
    if (ImGui::Button("Change model", ImVec2(-FLT_MIN, 0)))
        appState.screen = APP_SCREEN_SELECT_MODEL;
    ImGui::End();

    rlImGuiEnd();

}
