// Settings window: API key (masked, with a "Test key" check) and AI model.
#include "settings_ui.hpp"
#include <cstring>
#include <imgui.h>
#include <raylib.h>
#include "ai.hpp"
#include "settings.hpp"

namespace {

const ImVec4 kRed(1.0f, 0.38f, 0.38f, 1.0f), kGreen(0.45f, 0.95f, 0.55f, 1.0f), kAccent(0.45f, 0.75f, 1.0f, 1.0f);
constexpr const char* kTitle = "Settings";

bool open = false, openRequest = false, showKey = false;
char keyBuf[256], modelBuf[128];
std::string saveMsg;
bool saveErr = false;
double saveTime = -1e9;

const char* kPresets[] = {
    "anthropic/claude-sonnet-5.5",
    "anthropic/claude-opus-5.5",
    "anthropic/claude-haiku-5.5",
    "openai/gpt-6.1-sol",
    "google/gemini-3.8-flash",
};

void load() {
    std::snprintf(keyBuf, sizeof keyBuf, "%s", settingsApiKey().c_str());
    std::snprintf(modelBuf, sizeof modelBuf, "%s", settingsModel().c_str());
    saveMsg.clear();
    showKey = false;
}

bool save() {
    std::string err;
    saveErr = !settingsSave(keyBuf, modelBuf, err);
    saveMsg = saveErr ? err : "Saved.";
    saveTime = GetTime();
    return !saveErr;
}

} // namespace

void settingsUiRequestOpen() { openRequest = true; }
bool settingsUiModalOpen() { return open || openRequest; }

void settingsUiDraw() {
    if (openRequest) {
        openRequest = false;
        open = true;
        load();
        ImGui::OpenPopup(kTitle);
    }
    if (!open) return;
    ImGui::SetNextWindowPos(ImVec2(GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, &open, ImGuiWindowFlags_NoSavedSettings)) {
        open = false;
        return;
    }

    ImGui::TextColored(kAccent, "Hack Club AI");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480.0f);
    ImGui::TextDisabled("Used to turn a photo into a 3D model. Get a free key at ai.hackclub.com (it starts with sk-hc-v1-).");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    ImGui::TextUnformatted("API key");
    ImGui::SetNextItemWidth(-90);
    ImGuiInputTextFlags kf = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
    if (!showKey) kf |= ImGuiInputTextFlags_Password;
    if (ImGui::InputTextWithHint("##key", "sk-hc-v1-...", keyBuf, sizeof keyBuf, kf)) save();
    ImGui::SameLine();
    if (ImGui::Button(showKey ? "Hide" : "Show", ImVec2(80, 0))) showKey = !showKey;
    if (settingsKeyFromEnv() && !keyBuf[0]) ImGui::TextDisabled("Using HACKCLUB_API_KEY from the environment.");
    else if (!keyBuf[0]) ImGui::TextColored(kRed, "No key yet - photo to 3D won't work until you add one.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Model");
    ImGui::SetNextItemWidth(-90);
    ImGui::InputText("##model", modelBuf, sizeof modelBuf);
    ImGui::SameLine();
    if (ImGui::Button("Presets", ImVec2(80, 0))) ImGui::OpenPopup("##presets");
    if (ImGui::BeginPopup("##presets")) {
        for (const char* p : kPresets)
            if (ImGui::Selectable(p)) std::snprintf(modelBuf, sizeof modelBuf, "%s", p);
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("Must accept image input. Default: %s", kDefaultAiModel);

    ImGui::Spacing();
    AiTestStatus t = aiTestStatus();
    bool testing = t.state == AiTestState::Running;
    ImGui::BeginDisabled(testing || !keyBuf[0]);
    if (ImGui::Button("Test key", ImVec2(110, 0))) aiTestKey(keyBuf, modelBuf);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (testing) {
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(120, ImGui::GetFrameHeight()), "");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", t.message.c_str());
    } else if (t.state == AiTestState::Ok) {
        ImGui::TextColored(kGreen, "%s", t.message.c_str());
    } else if (t.state == AiTestState::Failed) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f);
        ImGui::TextColored(kRed, "%s", t.message.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Stored in ~/.wing-it/settings.json on this computer only.");
    if (ImGui::Button("Save", ImVec2(120, 0))) save();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(120, 0))) {
        open = false;
        ImGui::CloseCurrentPopup();
    }
    if (!saveMsg.empty() && GetTime() - saveTime < 6.0) {
        ImGui::SameLine();
        ImGui::TextColored(saveErr ? kRed : kGreen, "%s", saveMsg.c_str());
    }
    ImGui::EndPopup();
}
