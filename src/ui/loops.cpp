//
// Created by Adithiya Venkatakrishnan on 10/10/2026.
//

#include <imgui.h>
#include <rlImGui.h>
#include <thread>

#include "raylib.h"
#include "ui.hpp"
#include "title.hpp"
#include "models.hpp"
#include "solver.hpp"
#include <implot.h>
#include "multiplayer.hpp"
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
    if (ImGui::GetIO().WantTextInput) return; // typing in a text box (multiplayer name / IP) must not trigger hotkeys
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
    appState.frameCount++;
    if (appState.screen != APP_SCREEN_WELCOME) { // the title page has its own header
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::Begin("HUD", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoInputs);
        ImGui::TextDisabled("FRAME %lld   FPS %lld", appState.frameCount.load(), appState.frameRate.load());
        ImGui::End();
    }


    ImVec2 screenCenter = ImVec2(GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f);

    if (appState.screen == APP_SCREEN_WELCOME) {
        titleDraw();
        titleMenuBegin();
        if (titleMenuButton("Enter") || IsKeyPressed(KEY_ENTER))
            appState.screen = APP_SCREEN_SELECT_MODEL;
        // add more title menu buttons here, e.g. if (titleMenuButton("Multiplayer")) ...
        titleMenuEnd();
    } else if (appState.screen == APP_SCREEN_SELECT_MODEL) {
        ImGui::SetNextWindowPos(screenCenter, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin("Pick a model", nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_AlwaysAutoResize);

        if (ImGui::BeginListBox("##models", ImVec2(260, 0))) {
            for (int i = 0; i < (int)uiState.models.size(); ++i)
                if (ImGui::Selectable(modelLabel(uiState.models[i]).c_str(), uiState.model == i, ImGuiSelectableFlags_AllowDoubleClick)
                    && (uiState.model = i, ImGui::IsMouseDoubleClicked(0)))
                    uiState.modelChosen = true;
            ImGui::EndListBox();
        }
        if (!uiState.modelError.empty())
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", uiState.modelError.c_str());
        if (ImGui::Button("Back"))
            appState.screen = APP_SCREEN_WELCOME;
        ImGui::SameLine();
        if (ImGui::Button("Multiplayer")) // MP: 2-player battle
            mpEnter(appState);
        ImGui::SameLine();
        if (ImGui::Button("Start", ImVec2(-FLT_MIN, 0)) || IsKeyPressed(KEY_ENTER))
            uiState.modelChosen = !uiState.models.empty();

        ImGui::End();
    } else if (appState.screen >= APP_SCREEN_MP_LOBBY) {
        mpDrawUi(appState, uiState); // MP: lobby / pick / battle HUD / result
    }

    if (appState.screen != APP_SCREEN_UI) {
        rlImGuiEnd();
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(GetScreenWidth() - 10.0f, 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    {
        Forces fr = solverForces({}, {});
        ImGui::SetWindowFontScale(1.4f);
        ImGui::Text("Drag coefficient (Cd): %.3f", fr.cd);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Text("Lift (Cl): %.3f   Side (Cz): %.3f", fr.cl, fr.cz);
        float hist[400];
        int n = solverDragHistory(hist, 400);
        if (n > 1 && ImPlot::BeginPlot("##cd", ImVec2(260, 60), ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs)) {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 399, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 3, ImGuiCond_Once);
            ImPlot::PlotLine("Cd", hist, n);
            ImPlot::EndPlot();
        }
        ImGui::Separator();
    }
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
