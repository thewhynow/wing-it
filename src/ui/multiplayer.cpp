// Multiplayer UI + glue between the Match logic (multiplayer.hpp), the solver thread and main.cpp.
#include <atomic>
#include <cstdlib>
#include <imgui.h>
#include <raylib.h>
#include "multiplayer.hpp"
#include "render.hpp"
#include "solver.hpp"
#include "ui.hpp"
#include "models.hpp"
#include "model_library.hpp"

namespace {

mp::Match match;

// --- UI-only state ---
char nameBuf[32] = "";
char ipBuf[64] = "127.0.0.1";
std::string pickId;   // local model ID (a path). only modelLabel(pickId) goes over the wire (match.myModel)
glm::vec3 pickRot{0.0f};
bool lobbyInit = false;

// --- sim state, shared with the solver thread ---
MpSimRequest pendingReq;
bool reqPending = false;
std::atomic<bool> simActive{false}, simDone{false};
std::atomic<int> simSteps{0};
std::atomic<float> simCd{0.0f};      // Cd so far (held steady through flow restarts, see mpAfterStep)
std::atomic<bool> cdValid{false};    // simCd has a real sample yet
std::atomic<int> cdHoldUntil{0};     // don't sample Cd before this step (transient after a flow (re)start / avg reset)
int simTris = 1;
int lastSentPct = -1;
double lastCdSend = -100.0;
constexpr int kCdSettle = 100;       // steps

// --- halftime ---
std::atomic<bool> halftimeHit{false};   // solver thread -> UI: reached kHalfSteps, counting stopped
std::atomic<bool> halftimeUsed{false};  // halftime already happened this battle
std::atomic<float> halftimeCd{0.0f};
glm::vec3 curRot{0.0f};                 // rotation the current half runs with
glm::vec3 htRot{0.0f};                  // halftime sliders
glm::vec3 lastRotSent{0.0f};
double lastRotSend = -100.0;
glm::vec3 resumeRot{0.0f};
bool resumePending = false;             // UI -> main: rebuild with resumeRot and restart the solver

void resetSimFlags() {
    simActive = false;
    simDone = false;
    halftimeHit = false;
    halftimeUsed = false;
    cdValid = false;
    resumePending = false;
}

void backToMenu(AppState& app, UiState& ui, const std::string& msg) {
    resetSimFlags();
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
    if (modelIndexOf(ui, pickId) < 0) pickId = ui.models.empty() ? "" : ui.models[0];
    bool picked = modelLibraryList("##mpmodels", ui, pickId, ImVec2(-FLT_MIN, 200));
    picked |= modelLibraryControls(ui, pickId, !match.myReady); // add file / drag-and-drop / phone upload
    if (picked) match.setPick(modelLabel(pickId));
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
        ImGui::BeginDisabled(pickId.empty());
        if (ImGui::Button("Ready", ImVec2(-FLT_MIN, 0))) {
            match.setPick(modelLabel(pickId));
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
    else if (cdValid)
        ImGui::TextDisabled("Cd so far: %.4f%s", simCd.load(), match.myPastHalftime ? "  (2nd half)" : "");
    ImGui::Text("%s - %s", match.peer.name.c_str(), match.peer.model.c_str());
    ImGui::ProgressBar(match.peer.progress / 100.0f, ImVec2(-FLT_MIN, 0));
    if (match.peer.res.have) {
        ImGui::TextColored(kGreen, "Opponent finished.");
    } else {
        if (match.peer.haveCd) ImGui::TextDisabled("Cd so far: %.4f", match.peer.cd);
        if (match.peer.halftime) {
            if (match.peer.haveCd) ImGui::SameLine();
            ImGui::TextColored(kYellow, "At halftime");
        }
    }
    if (match.peerGone) ImGui::TextColored(kRed, "Opponent disconnected.");
    ImGui::Checkbox("Streamlines", &ui.mpShowLines); // local view preference only
    if (ImGui::Button("Forfeit / leave"))
        backToMenu(app, ui, "");
    ImGui::End();
}

const char* peerStatus() {
    const mp::Peer& p = match.peer;
    if (p.res.have) return "Finished";
    if (match.peerGone) return "Disconnected";
    if (p.halftime) return "At halftime";
    if (p.pastHalftime) return "Second half";
    return "First half";
}

void rotCell(bool have, glm::vec3 r) {
    if (have) ImGui::Text("%.0f / %.0f / %.0f", r.x, r.y, r.z);
    else ImGui::TextDisabled("-");
}

void centeredText(const char* s, bool disabled = false) {
    float w = ImGui::CalcTextSize(s).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowSize().x - w) * 0.5f));
    if (disabled) ImGui::TextDisabled("%s", s);
    else ImGui::TextUnformatted(s);
}

void drawHalftime(AppState& app, UiState& ui) {
    constexpr float kWinW = 640.0f;
    centeredWindow("Halftime", kWinW);

    ImGui::SetWindowFontScale(2.2f);
    centeredText("HALFTIME");
    ImGui::SetWindowFontScale(1.0f);
    char sub[96];
    std::snprintf(sub, sizeof sub, "First half complete  -  step %d of %d", mp::kHalfSteps, mp::kSteps);
    centeredText(sub, true);
    ImGui::Spacing();

    // top: live preview of the raw mesh at the slider rotation (nothing is voxelized until Resume) | rotation controls
    {
        const float pw = 340.0f, ph = std::max(120.0f, std::min(220.0f, GetScreenHeight() * 0.3f));
        ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        Color bgc = {(unsigned char)(bg.x * 255), (unsigned char)(bg.y * 255), (unsigned char)(bg.z * 255), 255};
        RenderTexture2D rt = renderMeshPreview((int)pw, (int)ph, htRot, bgc);
        ImGui::Image(ImTextureID(rt.texture.id), ImVec2(pw, ph), ImVec2(0, 1), ImVec2(1, 0)); // render textures are upside down
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Adjust your setup");
        ImGui::TextDisabled("Wind blows along +X (arrow).");
        ImGui::Spacing();
        ImGui::PushItemWidth(-ImGui::CalcTextSize("Rotate X").x - ImGui::GetStyle().ItemInnerSpacing.x);
        const char* axes[] = {"Rotate X", "Rotate Y", "Rotate Z"};
        for (int i = 0; i < 3; ++i)
            ImGui::SliderFloat(axes[i], &htRot[i], -180.0f, 180.0f, "%.0f deg");
        ImGui::PopItemWidth();
        if (ImGui::Button("Reset")) htRot = glm::vec3(0.0f);
        ImGui::SameLine();
        if (ImGui::Button("First-half rotation")) htRot = curRot;
        ImGui::EndGroup();
    }
    // let the opponent watch the adjustments (throttled)
    if (htRot != lastRotSent && GetTime() - lastRotSend > 0.15) {
        match.sendRotation(htRot);
        lastRotSent = htRot;
        lastRotSend = GetTime();
    }
    ImGui::Spacing();

    // scoreboard
    const mp::Peer& p = match.peer;
    if (ImGui::BeginTable("halftime_board", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Player"); ImGui::TableSetupColumn("Model"); ImGui::TableSetupColumn("Cd so far");
        ImGui::TableSetupColumn("Progress"); ImGui::TableSetupColumn("Status"); ImGui::TableSetupColumn("Rotation X/Y/Z");
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(match.myName.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(match.myModel.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.4f", halftimeCd.load());
        ImGui::TableNextColumn(); ImGui::Text("%d%%", std::min(100, simSteps.load() * 100 / mp::kSteps));
        ImGui::TableNextColumn(); ImGui::TextUnformatted("At halftime");
        ImGui::TableNextColumn(); rotCell(true, htRot);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(p.name.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(p.model.c_str());
        ImGui::TableNextColumn();
        if (p.res.have) ImGui::Text("%.4f (final)", p.res.cd);
        else if (p.haveCd) ImGui::Text("%.4f", p.cd);
        else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%d%%", p.progress);
        ImGui::TableNextColumn();
        if (match.peerGone && !p.res.have) ImGui::TextColored(kRed, "%s", peerStatus());
        else ImGui::TextUnformatted(peerStatus());
        ImGui::TableNextColumn(); rotCell(p.haveRot, p.rot);
        ImGui::EndTable();
    }
    if (match.peerGone)
        ImGui::TextColored(kRed, "Opponent disconnected after finishing - you can still complete your run.");
    else if (p.halftime)
        ImGui::TextDisabled("%s is at halftime too.", p.name.c_str());
    else if (!p.res.have)
        ImGui::TextDisabled("%s is still racing - nobody waits at halftime.", p.name.c_str());

    ImGui::TextDisabled("Resuming restarts the flow around the new orientation; final Cd = mean of the last %d steps.",
                        mp::kAvgWindow);
    ImGui::Spacing();
    const float pw = kWinW - 2.0f * ImGui::GetStyle().WindowPadding.x;
    if (ImGui::Button("Resume second half", ImVec2(pw * 0.7f, 0))) {
        resumeRot = htRot;
        resumePending = true;
        curRot = htRot;
        match.leaveHalftime(htRot);
        app.screen = APP_SCREEN_MP_BATTLE;
    }
    ImGui::SameLine();
    bool forfeit = ImGui::Button("Forfeit", ImVec2(-FLT_MIN, 0));
    ImGui::End();
    if (forfeit)
        backToMenu(app, ui, "");
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
    resetSimFlags();
    reqPending = false;
    pickId.clear();
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
                if (modelIndexOf(ui, pickId) < 0) pickId = ui.models.empty() ? "" : ui.models[0];
                if (!pickId.empty()) match.setPick(modelLabel(pickId));
            }
            break;
        case mp::Match::Battle: app.screen = match.myHalftime ? APP_SCREEN_MP_HALFTIME : APP_SCREEN_MP_BATTLE; break;
        case mp::Match::Result: app.screen = APP_SCREEN_MP_RESULT; break;
    }
    if (match.takeStart() && !pickId.empty()) {
        pendingReq = {pickId, pickRot}; // local ID; match.myModel is only the display label
        reqPending = true;
        resetSimFlags(); simSteps = 0; lastSentPct = -1; lastCdSend = -100.0;
        curRot = htRot = lastRotSent = pickRot;
        match.sendRotation(pickRot);
    }
    if (match.phase == mp::Match::Battle) {
        // solver thread reached kHalfSteps and stopped counting: go to the halftime screen (main then stops the solver)
        if (halftimeHit.exchange(false) && !match.myHalftime) {
            match.enterHalftime(halftimeCd);
            htRot = lastRotSent = curRot;
            app.screen = APP_SCREEN_MP_HALFTIME;
        }
        int pct = std::min(100, simSteps.load() * 100 / mp::kSteps);
        if (simActive && pct != lastSentPct) { lastSentPct = pct; match.sendProgress(pct); }
        if (simActive && cdValid && GetTime() - lastCdSend > 0.5) { lastCdSend = GetTime(); match.sendCd(simCd); }
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
        case APP_SCREEN_MP_HALFTIME: drawHalftime(app, ui); break;
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
    resetSimFlags(); // solver thread is stopped here
    simSteps = 0;
    simCd = 0.0f;
    halftimeCd = 0.0f;
    cdHoldUntil = kCdSettle;
    solverResetDragAverage();
    simActive = true; // main starts the solver thread right after this
}

bool mpTakeResume(glm::vec3& rotationDeg) {
    if (!resumePending) return false;
    resumePending = false;
    rotationDeg = resumeRot;
    return true;
}

void mpSimResume() {
    // solver is stopped and buildModel's solverReset just cleared the drag average. The step count carries on from
    // kHalfSteps, so the final averaging window in mpAfterStep is unchanged; "Cd so far" holds the halftime value
    // while the restarted flow gets past its start-up transient.
    simCd = halftimeCd.load();
    cdHoldUntil = simSteps.load() + kCdSettle;
    simActive = true;
}

void mpSimFailed(const std::string& why) {
    match.fatal = why; // picked up by the next mpDrawUi
}

// solver thread, once per step
void mpAfterStep() {
    if (!simActive) return;
    int s = ++simSteps;
    constexpr int windowStart = mp::kSteps - mp::kAvgWindow;
    if (s == windowStart) {
        solverResetDragAverage();
        cdHoldUntil = s + std::min(kCdSettle, mp::kAvgWindow / 2); // keep reporting the previous value for a bit
    }
    if (s >= cdHoldUntil && s % 10 == 0) {
        simCd = solverDragCoefficient();
        cdValid = true;
    }
    if (s == mp::kHalfSteps && !halftimeUsed) {
        // halftime: stop counting. The UI picks up halftimeHit, main stops the solver behind the halftime screen,
        // and any steps this thread runs before that are discarded by the rebuild on resume.
        float cd = solverDragCoefficient();
        halftimeCd = cd;
        simCd = cd;
        cdValid = true;
        halftimeUsed = true;
        simActive = false;
        halftimeHit = true;
        return;
    }
    if (s >= mp::kSteps) {
        simCd = solverDragCoefficient();
        simActive = false;
        simDone = true; // solver keeps stepping so the live view stays alive
    }
}

void mpShutdown() { match.net.close(); }
