#pragma once
// 2-player "aerodynamics battle". Both players run the SAME fixed wind-tunnel conditions on their own machine,
// each simulating only their own model, then exchange {cd, triangles, model}. Score = cd / triangles, GREATER wins.
// ASSUMPTION: both clients have the same assets/models (only the model *name* crosses the wire, for display).
//
// Protocol: one message per line, fields separated by TAB:
//   HELLO v name | PICK model | READY model | PROGRESS pct | RESULT cd tris model | REMATCH | BYE
// The Match class below is pure logic (no raylib/imgui) so it can be tested headlessly.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "net.hpp"

struct AppState;
struct UiState;

namespace mp {

// ---- predetermined battle conditions (single place to tweak) ----
constexpr int   kPort      = 7777;
constexpr int   kGridSize  = 64;     // tunnel is 2n x n x n
constexpr float kInletU    = 0.06f;
constexpr float kInletRho  = 1.0f;
constexpr float kTau       = 0.55f;
constexpr float kAoaDeg    = 0.0f;
constexpr int   kSteps     = 1500;   // fixed step count
constexpr int   kAvgWindow = 300;    // Cd averaged over the last kAvgWindow steps
constexpr int   kProtocol  = 1;

struct Result {
    bool have = false;
    float cd = 0.0f;
    int tris = 1;
    std::string model;
    double score() const { return tris > 0 ? (double)cd / tris : 0.0; }
};

enum class Verdict { Win, Lose, Draw };

// symmetric: both clients call it with (mine, theirs) / (theirs, mine) and agree. Greater score wins.
inline Verdict verdict(const Result& me, const Result& opp) {
    double a = me.score(), b = opp.score();
    double eps = 1e-12 * std::fmax(std::fabs(a), std::fabs(b));
    if (std::fabs(a - b) <= eps) return Verdict::Draw;
    return a > b ? Verdict::Win : Verdict::Lose;
}

inline std::string clean(std::string s) {
    for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

inline std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    for (;;) {
        size_t j = s.find('\t', i);
        out.push_back(s.substr(i, j == std::string::npos ? j : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return out;
}

struct Peer {
    std::string name = "Opponent", model;
    bool picked = false, ready = false, rematch = false;
    int progress = 0; // percent
    Result res;
};

class Match {
public:
    enum Phase { Lobby, Pick, Battle, Result };

    NetPeer net;
    Phase phase = Lobby;
    std::string myName = "Player";
    std::string myModel;
    bool myReady = false, myRematch = false, sentResult = false;
    mp::Result myRes;
    Peer peer;
    bool peerGone = false;        // peer disconnected/left (only tolerated once its result is in / on result screen)
    bool startPending = false;    // UI/main should start the local simulation (consumed by takeStart)
    std::string fatal;            // non-empty: UI should drop back to the menu with this message
    std::string lobbyError;       // shown in lobby (connect failures)

    void hostGame(int port = kPort) { resetAll(); net.host(port); }
    void joinGame(const std::string& addr, int port = kPort) { resetAll(); net.join(addr, port); }

    void leave() { // polite goodbye, then drop everything
        if (net.state() == NetState::Connected) net.send("BYE");
        net.close();
        resetAll();
    }

    void setPick(const std::string& model) {
        myModel = clean(model);
        if (phase == Pick && !myReady) net.send("PICK\t" + myModel);
    }
    void setReady() {
        if (phase != Pick || myReady || myModel.empty()) return;
        myReady = true;
        net.send("READY\t" + myModel);
    }
    bool takeStart() { bool s = startPending; startPending = false; return s; }

    void sendProgress(int pct) { if (phase == Battle) net.send("PROGRESS\t" + std::to_string(pct)); }
    // local sim finished: raw (pre-rotation) mesh triangle count
    void finished(float cd, int tris) {
        if (phase != Battle || sentResult) return;
        if (!std::isfinite(cd)) cd = 0.0f;
        if (tris < 1) tris = 1;
        myRes = {true, cd, tris, myModel};
        sentResult = true;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.9g", (double)cd);
        net.send(std::string("RESULT\t") + buf + "\t" + std::to_string(tris) + "\t" + myModel);
    }
    void requestRematch() {
        if (phase != Result || myRematch || peerGone) return;
        myRematch = true;
        net.send("REMATCH");
    }

    // call once per frame from the render thread: drains messages, advances the phase
    void update() {
        std::string line;
        while (net.pop(line)) handle(line);

        NetState st = net.state();
        if (phase == Lobby) {
            if (st == NetState::Failed || st == NetState::Closed) {
                lobbyError = net.error();
                net.close();
                helloSent = gotHello = false;
            } else if (st == NetState::Connected) {
                if (!helloSent) {
                    helloSent = true;
                    net.send("HELLO\t" + std::to_string(kProtocol) + "\t" + clean(myName));
                }
                if (gotHello) enterPick(false);
            }
            return;
        }

        if (st == NetState::Failed || st == NetState::Closed) peerGone = true;
        if (peerGone && !peerGoneOk()) {
            if (fatal.empty()) fatal = !peerLeftNote.empty() ? peerLeftNote : net.error();
            return;
        }

        if (phase == Pick && myReady && peer.ready) {
            phase = Battle;
            sentResult = false;
            myRes = {};
            peer.res = {};
            peer.progress = 0;
            startPending = true;
        }
        if (phase == Battle && myRes.have && peer.res.have) phase = Result;
        if (phase == Result && myRematch && peer.rematch) enterPick(true);
    }

    std::string peerLeftNote; // set when peer sent BYE

private:
    bool helloSent = false, gotHello = false;

    // after the peer is gone the match can only continue if nothing more is needed from it
    bool peerGoneOk() const {
        if (phase == Result) return true;
        if (phase == Battle && peer.res.have) return true;
        return false;
    }

    void resetAll() {
        phase = Lobby; myModel.clear(); myReady = myRematch = sentResult = false; myRes = {};
        peer = {}; peerGone = false; startPending = false; fatal.clear(); lobbyError.clear();
        peerLeftNote.clear(); helloSent = gotHello = false;
    }

    void enterPick(bool rematch) {
        phase = Pick;
        std::string keepName = peer.name;
        peer = {};
        peer.name = keepName;
        myReady = myRematch = sentResult = false;
        myRes = {};
        (void)rematch;
        if (!myModel.empty()) net.send("PICK\t" + myModel);
    }

    void handle(const std::string& line) {
        auto f = split(line);
        const std::string& c = f[0];
        if (c == "HELLO" && f.size() >= 3) {
            if (std::atoi(f[1].c_str()) != kProtocol) { fatal = "opponent runs an incompatible version"; return; }
            peer.name = f[2].empty() ? "Opponent" : f[2];
            gotHello = true;
        } else if (c == "PICK" && f.size() >= 2) {
            peer.model = f[1]; peer.picked = true;
        } else if (c == "READY" && f.size() >= 2) {
            peer.model = f[1]; peer.picked = true; peer.ready = true;
        } else if (c == "PROGRESS" && f.size() >= 2) {
            peer.progress = std::max(0, std::min(100, std::atoi(f[1].c_str())));
        } else if (c == "RESULT" && f.size() >= 4) {
            float cd = std::strtof(f[1].c_str(), nullptr);
            if (!std::isfinite(cd)) cd = 0.0f;
            int tris = std::max(1, std::atoi(f[2].c_str()));
            peer.res = {true, cd, tris, f[3]};
            peer.progress = 100;
        } else if (c == "REMATCH") {
            peer.rematch = true;
        } else if (c == "BYE") {
            peerGone = true;
            peerLeftNote = "opponent left the match";
        }
    }
};

} // namespace mp

// ---- glue used by the app (src/ui/multiplayer.cpp) ----
struct MpSimRequest { std::string model; glm::vec3 rotation{0.0f}; };

void mpEnter(AppState& app);                       // "Multiplayer" button: go to the lobby
void mpDrawUi(AppState& app, UiState& ui);         // call inside rlImGuiBegin/End for screen >= APP_SCREEN_MP_LOBBY
bool mpTakeSimRequest(MpSimRequest& req);          // main: true once when a battle sim should start
void mpSimBegin(int triangles);                    // main: after the model is built, just before the solver starts
void mpSimFailed(const std::string& why);          // main: model could not be loaded
void mpAfterStep();                                // solver thread: call after every solverStep
void mpShutdown();                                 // on exit: close sockets
