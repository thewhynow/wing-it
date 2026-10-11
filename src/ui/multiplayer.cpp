// Multiplayer UI + glue between the Match logic (multiplayer.hpp), the solver thread and main.cpp.
#include <atomic>
#include <cstdlib>
#include <imgui.h>
#include <raylib.h>
#include "multiplayer.hpp"
#include "solver.hpp"
#include "ui.hpp"

namespace {

mp::Match match;

// --- UI-only state ---
char nameBuf[32] = "";
char ipBuf[64] = "127.0.0.1";
int pickIdx = 0;
glm::vec3 pickRot{0.0f};
bool lobbyInit = false;

// --- sim state, shared with the solver thread ---
MpSimRequest pendingReq;
bool reqPending = false;
std::atomic<bool> simActive{false}, simDone{false};
std::atomic<int> simSteps{0};
std::atomic<float> simCd{0.0f};
int simTris = 1;
int lastSentPct = -1;

void backToMenu(AppState& app, UiState& ui, const std::string& msg) {
    simActive = false;
    reqPending = false;
    match.leave();
    ui.modelError = msg;
    app.screen = APP_SCREEN_SELECT_MODEL;
}

const ImVec4 kRed(1, 0.3f, 0.3f, 1), kGreen(0.4f, 1, 0.5f, 1), kYellow(1, 0.85f, 0.3f, 1);

void centeredWindow(const char* title, float w = 380.0f) {
    ImGui::SetNextWindowPos(ImVec2(GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
}

void drawLobby(AppState& app, UiState& ui) {
    centeredWindow("Multiplayer lobby");
    NetState st = match.net.state();
    if (st == NetState::Idle) {
        ImGui::InputText("Your name", nameBuf, sizeof nameBuf);
        if (!match.lobbyError.empty())
            ImGui::TextColored(kRed, "%s", match.lobbyError.c_str());
        ImGui::Separator();
        if (ImGui::Button("Host game (port 7777)", ImVec2(-FLT_MIN, 0))) {
            match.myName = nameBuf[0] ? nameBuf : "Player";
            match.hostGame(mp::kPort);
        }
        ImGui::Spacing();
        ImGui::InputText("Host IP", ipBuf, sizeof ipBuf);
        if (ImGui::Button("Join game", ImVec2(-FLT_MIN, 0))) {
            match.myName = nameBuf[0] ? nameBuf : "Player";
            match.joinGame(ipBuf, mp::kPort);
        }
        ImGui::Spacing();
        if (ImGui::Button("Back"))
            backToMenu(app, ui, "");
    } else {
        if (st == NetState::Listening) {
            ImGui::Text("Waiting for an opponent on port %d...", mp::kPort);
            ImGui::TextDisabled("Tell them one of your addresses:");
            static std::vector<std::string> addrs;
            static double lastScan = -100;
            if (GetTime() - lastScan > 3.0) { addrs = NetPeer::localAddresses(); lastScan = GetTime(); }
            for (auto& a : addrs) ImGui::BulletText("%s", a.c_str());
            if (addrs.empty()) ImGui::BulletText("(no network interface found - try 127.0.0.1 locally)");
        } else if (st == NetState::Connecting) {
            ImGui::Text("Connecting to %s...", ipBuf);
        } else {
            ImGui::Text("Connected, saying hello...");
        }
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0))) {
            match.net.close();
            match.lobbyError.clear();
        }
    }
    ImGui::End();
}

void drawPick(AppState& app, UiState& ui) {
    centeredWindow("Pick your model", 420.0f);
    ImGui::Text("You: %s", match.myName.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("vs");
    ImGui::SameLine();
    ImGui::Text("%s", match.peer.name.c_str());

    // opponent status
    if (match.peer.ready)
        ImGui::TextColored(kGreen, "%s is READY (%s)", match.peer.name.c_str(), match.peer.model.c_str());
    else if (match.peer.picked)
        ImGui::TextColored(kYellow, "%s picked a model", match.peer.name.c_str());
    else
        ImGui::TextDisabled("%s is choosing...", match.peer.name.c_str());
    ImGui::Separator();

    ImGui::BeginDisabled(match.myReady);
    if (ImGui::BeginListBox("##mpmodels", ImVec2(-FLT_MIN, 150))) {
        for (int i = 0; i < (int)ui.models.size(); ++i)
            if (ImGui::Selectable(ui.models[i].c_str(), pickIdx == i) && pickIdx != i) {
                pickIdx = i;
                match.setPick(ui.models[i]);
            }
        ImGui::EndListBox();
    }
    const char* axes[] = {"Rotate X", "Rotate Y", "Rotate Z"};
    for (int i = 0; i < 3; ++i)
        ImGui::SliderFloat(axes[i], &pickRot[i], -180.0f, 180.0f, "%.0f deg");
    ImGui::EndDisabled();

    ImGui::TextDisabled("Fixed conditions: grid %d, wind %.2f, tau %.2f, %d steps, AoA %.0f",
                        mp::kGridSize, mp::kInletU, mp::kTau, mp::kSteps, mp::kAoaDeg);
    ImGui::TextDisabled("Score = Cd / triangles. Greater score wins.");

    if (match.myReady) {
        ImGui::TextColored(kGreen, "Ready - waiting for %s...", match.peer.name.c_str());
    } else {
        ImGui::BeginDisabled(ui.models.empty());
        if (ImGui::Button("Ready", ImVec2(-FLT_MIN, 0))) {
            match.setPick(ui.models[pickIdx]);
            match.setReady();
        }
        ImGui::EndDisabled();
    }
    if (ImGui::Button("Leave"))
        backToMenu(app, ui, "");
    ImGui::End();
}

void drawBattleHud(AppState& app, UiState& ui) {
    ImGui::SetNextWindowPos(ImVec2(GetScreenWidth() * 0.5f, 10.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Always);
    ImGui::Begin("Battle", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    float mine = std::min(1.0f, simSteps.load() / (float)mp::kSteps);
    ImGui::Text("You (%s) - %s", match.myName.c_str(), match.myModel.c_str());
    ImGui::ProgressBar(mine, ImVec2(-FLT_MIN, 0));
    if (simDone && match.myRes.have)
        ImGui::TextColored(kGreen, "Done: Cd %.4f, %d tris. Waiting for opponent...", match.myRes.cd, match.myRes.tris);
    ImGui::Text("%s - %s", match.peer.name.c_str(), match.peer.model.c_str());
    ImGui::ProgressBar(match.peer.progress / 100.0f, ImVec2(-FLT_MIN, 0));
    if (match.peer.res.have) ImGui::TextColored(kGreen, "Opponent finished.");
    if (match.peerGone) ImGui::TextColored(kRed, "Opponent disconnected.");
    if (ImGui::Button("Forfeit / leave"))
        backToMenu(app, ui, "");
    ImGui::End();
}

void resultRow(const char* who, const mp::Result& r) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextUnformatted(who);
    ImGui::TableNextColumn(); ImGui::TextUnformatted(r.model.c_str());
    ImGui::TableNextColumn(); ImGui::Text("%.4f", r.cd);
    ImGui::TableNextColumn(); ImGui::Text("%d", r.tris);
    ImGui::TableNextColumn(); ImGui::Text("%.3e", r.score());
}

void drawResult(AppState& app, UiState& ui) {
    centeredWindow("Result", 560.0f);
    mp::Verdict v = mp::verdict(match.myRes, match.peer.res);
    ImGui::SetWindowFontScale(2.0f);
    if (v == mp::Verdict::Win) ImGui::TextColored(kGreen, "YOU WIN!");
    else if (v == mp::Verdict::Lose) ImGui::TextColored(kRed, "%s WINS", match.peer.name.c_str());
    else ImGui::TextColored(kYellow, "IT'S A DRAW");
    ImGui::SetWindowFontScale(1.0f);
    if (ImGui::BeginTable("res", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Player"); ImGui::TableSetupColumn("Model");
        ImGui::TableSetupColumn("Cd"); ImGui::TableSetupColumn("Triangles"); ImGui::TableSetupColumn("Score (Cd/tri)");
        ImGui::TableHeadersRow();
        resultRow(match.myName.c_str(), match.myRes);
        resultRow(match.peer.name.c_str(), match.peer.res);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Greater score wins.");
    if (match.peerGone) {
        ImGui::TextColored(kRed, "Opponent left - no rematch possible.");
    } else if (match.myRematch) {
        ImGui::TextColored(kYellow, "Waiting for opponent to accept rematch...");
    } else if (match.peer.rematch) {
        ImGui::TextColored(kGreen, "Opponent wants a rematch!");
    }
    ImGui::BeginDisabled(match.peerGone || match.myRematch);
    if (ImGui::Button("Rematch")) match.requestRematch();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Back to menu"))
        backToMenu(app, ui, "");
    ImGui::End();
}

} // namespace

void mpEnter(AppState& app) {
    if (!lobbyInit) {
        lobbyInit = true;
        const char* u = std::getenv("USER");
        std::snprintf(nameBuf, sizeof nameBuf, "%s", u && *u ? u : "Player");
    }
    match.leave();
    pickIdx = 0;
    pickRot = glm::vec3(0.0f);
    app.screen = APP_SCREEN_MP_LOBBY;
}

void mpDrawUi(AppState& app, UiState& ui) {
    // --- advance the match ---
    match.update();
    if (!match.fatal.empty()) {
        backToMenu(app, ui, "Multiplayer: " + match.fatal);
        return;
    }
    switch (match.phase) {
        case mp::Match::Lobby:  app.screen = APP_SCREEN_MP_LOBBY; break;
        case mp::Match::Pick:
            if (app.screen != APP_SCREEN_MP_PICK) {
                app.screen = APP_SCREEN_MP_PICK;
                pickIdx = std::min(pickIdx, std::max(0, (int)ui.models.size() - 1));
                if (!ui.models.empty()) match.setPick(ui.models[pickIdx]);
            }
            break;
        case mp::Match::Battle: app.screen = APP_SCREEN_MP_BATTLE; break;
        case mp::Match::Result: app.screen = APP_SCREEN_MP_RESULT; break;
    }
    if (match.takeStart() && !ui.models.empty()) {
        pendingReq = {match.myModel, pickRot};
        reqPending = true;
        simDone = false; simActive = false; simSteps = 0; lastSentPct = -1;
    }
    if (match.phase == mp::Match::Battle) {
        int pct = std::min(100, simSteps.load() * 100 / mp::kSteps);
        if (simActive && pct != lastSentPct) { lastSentPct = pct; match.sendProgress(pct); }
        if (simDone && !match.myRes.have) {
            match.finished(simCd, simTris);
            match.update(); // maybe the opponent was already done
        }
    }

    switch (app.screen) {
        case APP_SCREEN_MP_LOBBY:  drawLobby(app, ui); break;
        case APP_SCREEN_MP_PICK:   drawPick(app, ui); break;
        case APP_SCREEN_MP_BATTLE: drawBattleHud(app, ui); break;
        case APP_SCREEN_MP_RESULT: drawResult(app, ui); break;
        default: break;
    }
}

bool mpTakeSimRequest(MpSimRequest& req) {
    if (!reqPending) return false;
    reqPending = false;
    req = pendingReq;
    return true;
}

void mpSimBegin(int triangles) {
    simTris = triangles;
    simSteps = 0;
    simDone = false;
    simCd = 0.0f;
    solverResetDragAverage();
    simActive = true; // main starts the solver thread right after this
}

void mpSimFailed(const std::string& why) {
    match.fatal = why; // picked up by the next mpDrawUi
}

// solver thread, once per step
void mpAfterStep() {
    if (!simActive) return;
    int s = ++simSteps;
    constexpr int windowStart = mp::kSteps - mp::kAvgWindow;
    if (s == windowStart) solverResetDragAverage();
    if (s >= windowStart && (s % 10 == 0 || s == mp::kSteps))
        simCd = solverDragCoefficient();
    if (s >= mp::kSteps) {
        simCd = solverDragCoefficient();
        simActive = false;
        simDone = true; // solver keeps stepping so the live view stays alive
    }
}

void mpShutdown() { match.net.close(); }
