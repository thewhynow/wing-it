#pragma once
// 2-player "aerodynamics battle". Both players run the SAME fixed wind-tunnel conditions on their own machine,
// each simulating only their own model, then exchange {cd, triangles, model}. Score = cd / triangles, GREATER wins.
// ASSUMPTION: both clients have the same assets/models (only the model *name* crosses the wire, for display).
//
// Protocol: one message per line, fields separated by TAB:
//   HELLO v name | PICK model | READY model | PROGRESS pct | RESULT cd tris model | REMATCH | BYE
//   CD cd            sender's drag coefficient so far (sent periodically during the battle)
//   HALF cd          sender reached halftime (kHalfSteps) and paused; cd = its Cd so far
//   ROT x y z        sender's model rotation in degrees (battle start, live while at halftime)
//   RESUME x y z     sender left halftime with this rotation and is racing the second half
// Halftime: at kHalfSteps each side pauses on its own, may re-rotate its model, and resumes (re-voxelize +
// flow restart, step counter keeps going). Nobody waits for anybody; scoring is unchanged.
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

constexpr int   kPort      = 7777;
constexpr int   kGridSize  = 64;
constexpr float kInletU    = 0.06f;
constexpr float kInletRho  = 1.0f;
constexpr float kTau       = 0.55f;
constexpr float kAoaDeg    = 0.0f;
constexpr int   kSteps     = 1500;
constexpr int   kAvgWindow = 300;
constexpr int   kHalfSteps = kSteps / 2;
constexpr int   kProtocol  = 2;
static_assert(kHalfSteps < kSteps - kAvgWindow, "halftime must come before the Cd averaging window");

enum class Goal {
    Aero = 0,
    Drag = 1
};

inline const char* goalName(Goal g) {
    return g == Goal::Aero ? "Maximize Aerodynamics" : "Maximize Drag";
}

struct Result {
    bool have = false;
    float cd = 0.0f;
    int tris = 1;
    std::string model;
    double score() const { return tris > 0 ? (double)cd / tris : 0.0; }
};

enum class Verdict { Win, Lose, Draw };

// Symmetric verdict: greater score wins for Drag, least score wins for Aero
inline Verdict verdict(const Result& me, const Result& opp, Goal g = Goal::Drag) {
    double a = me.score(), b = opp.score();
    double eps = 1e-12 * std::fmax(std::fabs(a), std::fabs(b));
    if (std::fabs(a - b) <= eps) return Verdict::Draw;
    if (g == Goal::Aero) {
        return a < b ? Verdict::Win : Verdict::Lose;
    } else {
        return a > b ? Verdict::Win : Verdict::Lose;
    }
}

struct PlayerScore {
    int id = 0;
    std::string name;
    bool isMe = false;
    Result res;
    int rank = 1;
};

inline std::vector<PlayerScore> rankPlayers(const std::vector<PlayerScore>& players, Goal g) {
    auto sorted = players;
    std::stable_sort(sorted.begin(), sorted.end(), [g](const PlayerScore& a, const PlayerScore& b) {
        if (!a.res.have && !b.res.have) return false;
        if (!a.res.have) return false; // DNF goes last
        if (!b.res.have) return true;
        if (g == Goal::Aero) {
            return a.res.score() < b.res.score();
        } else {
            return a.res.score() > b.res.score();
        }
    });
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i == 0) {
            sorted[i].rank = 1;
        } else {
            double prev = sorted[i - 1].res.score();
            double cur = sorted[i].res.score();
            double eps = 1e-12 * std::fmax(std::fabs(prev), std::fabs(cur));
            if (sorted[i].res.have && sorted[i - 1].res.have && std::fabs(prev - cur) <= eps) {
                sorted[i].rank = sorted[i - 1].rank;
            } else {
                sorted[i].rank = (int)(i + 1);
            }
        }
    }
    return sorted;
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
    int id = 0;
    std::string name = "Opponent", model;
    bool isHost = false;
    bool picked = false, ready = false, rematch = false;
    int progress = 0; // percent
    Result res;
    // halftime / live status (all optional: an older peer simply never sends these)
    bool haveCd = false;
    float cd = 0.0f;              // Cd so far
    bool halftime = false;        // currently paused at halftime
    bool pastHalftime = false;    // resumed into the second half
    float halftimeCd = 0.0f;
    bool haveRot = false;
    glm::vec3 rot{0.0f};          // degrees
    bool gone = false;            // disconnected / left
};

inline bool parseRot(const std::vector<std::string>& f, glm::vec3& out, size_t startIdx = 1) {
    if (f.size() < startIdx + 3) return false;
    glm::vec3 r;
    for (int i = 0; i < 3; ++i) {
        r[i] = std::strtof(f[startIdx + i].c_str(), nullptr);
        if (!std::isfinite(r[i])) return false;
        r[i] = std::max(-180.0f, std::min(180.0f, r[i]));
    }
    out = r;
    return true;
}

inline std::string fmtFloat(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", (double)(std::isfinite(v) ? v : 0.0f));
    return buf;
}

class Match {
public:
    enum Phase { Lobby, Pick, Battle, Result };

    NetPeer net;
    Phase phase = Lobby;
    Goal goal = Goal::Drag;       // host chooses Drag or Aero; default Drag keeps legacy test parity
    bool isHost = false;
    int myId = 0;
    bool manualStart = false;     // true in UI so host can wait for up to 4 players

    std::string myName = "Player";
    std::string myModel;
    bool myReady = false, myRematch = false, sentResult = false;
    bool myHalftime = false;      // I'm paused at halftime (sub-state of Battle)
    bool myPastHalftime = false;  // I've resumed into the second half
    mp::Result myRes;

    Peer peer;                    // first peer, always kept equal to peers[0] for 2-player backwards compatibility
    std::vector<Peer> peers;      // all connected opponents (up to 3)
    bool peerGone = false;        // peer disconnected/left
    bool startPending = false;    // UI/main should start the local simulation (consumed by takeStart)
    std::string fatal;            // non-empty: UI should drop back to the menu with this message
    std::string lobbyError;       // shown in lobby (connect failures)
    std::string peerLeftNote;     // set when peer sent BYE

    void setGoal(Goal g) {
        goal = g;
        if (isHost && net.state() == NetState::Connected) {
            net.send("GOAL\t" + std::to_string((int)goal));
        }
    }

    void hostGame(int port = kPort) {
        resetAll();
        isHost = true;
        myId = 0;
        net.host(port, NetPeer::kMaxClients);
    }

    void joinGame(const std::string& addr, int port = kPort) {
        resetAll();
        isHost = false;
        myId = -1;
        net.join(addr, port);
    }

    void leave() {
        if (net.state() == NetState::Connected) net.send("BYE");
        net.close();
        resetAll();
    }

    void startPick() {
        if (!isHost || phase != Lobby || peers.empty()) return;
        net.send("START_PICK");
        enterPick(false);
    }

    void setPick(const std::string& model) {
        myModel = clean(model);
        if (phase == Pick && !myReady) {
            if (isHost) net.send("PICK\t0\t" + myModel);
            else net.send("PICK\t" + myModel);
        }
    }

    void setReady() {
        if (phase != Pick || myReady || myModel.empty()) return;
        myReady = true;
        if (isHost) net.send("READY\t0\t" + myModel);
        else net.send("READY\t" + myModel);
    }

    bool takeStart() { bool s = startPending; startPending = false; return s; }

    void sendProgress(int pct) {
        if (phase == Battle) {
            if (isHost) net.send("PROGRESS\t0\t" + std::to_string(pct));
            else net.send("PROGRESS\t" + std::to_string(pct));
        }
    }

    void sendCd(float cd) {
        if (phase == Battle) {
            if (isHost) net.send("CD\t0\t" + fmtFloat(cd));
            else net.send("CD\t" + fmtFloat(cd));
        }
    }

    void sendRotation(glm::vec3 r) {
        if (phase == Battle) {
            std::string payload = fmtFloat(r.x) + "\t" + fmtFloat(r.y) + "\t" + fmtFloat(r.z);
            if (isHost) net.send("ROT\t0\t" + payload);
            else net.send("ROT\t" + payload);
        }
    }

    // local sim hit kHalfSteps and paused
    void enterHalftime(float cdSoFar) {
        if (phase != Battle || myHalftime || myPastHalftime || sentResult) return;
        myHalftime = true;
        if (isHost) net.send("HALF\t0\t" + fmtFloat(cdSoFar));
        else net.send("HALF\t" + fmtFloat(cdSoFar));
    }

    // leaving halftime with (possibly) a new rotation
    void leaveHalftime(glm::vec3 r) {
        if (phase != Battle || !myHalftime) return;
        myHalftime = false;
        myPastHalftime = true;
        std::string payload = fmtFloat(r.x) + "\t" + fmtFloat(r.y) + "\t" + fmtFloat(r.z);
        if (isHost) net.send("RESUME\t0\t" + payload);
        else net.send("RESUME\t" + payload);
    }

    // local sim finished: raw (pre-rotation) mesh triangle count
    void finished(float cd, int tris) {
        if (phase != Battle || sentResult) return;
        if (!std::isfinite(cd)) cd = 0.0f;
        if (tris < 1) tris = 1;
        myRes = {true, cd, tris, myModel};
        sentResult = true;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.9g", (double)cd);
        std::string payload = std::string(buf) + "\t" + std::to_string(tris) + "\t" + myModel;
        if (isHost) net.send("RESULT\t0\t" + payload);
        else net.send("RESULT\t" + payload);
    }

    void requestRematch() {
        if (phase != Result || myRematch || peerGone) return;
        myRematch = true;
        if (isHost) net.send("REMATCH\t0");
        else net.send("REMATCH");
    }

    size_t peerCount() const { return peers.size(); }
    size_t activePeerCount() const {
        size_t c = 0;
        for (const auto& p : peers) if (!p.gone) ++c;
        return c;
    }

    // call once per frame from the render thread: drains messages, advances the phase
    void update() {
        int senderId = 0;
        std::string line;
        while (net.pop(senderId, line)) handle(senderId, line);
        syncPeer();

        NetState st = net.state();
        if (phase == Lobby) {
            if (st == NetState::Failed || st == NetState::Closed) {
                lobbyError = net.error();
                net.close();
                helloSent = gotHello = false;
            } else if (st == NetState::Connected) {
                if (!isHost && !helloSent) {
                    helloSent = true;
                    net.send("HELLO\t" + std::to_string(kProtocol) + "\t" + clean(myName));
                }
                if (!isHost && gotHello && !manualStart) enterPick(false);
            }
            return;
        }

        if (st == NetState::Failed || st == NetState::Closed) {
            if (!isHost) {
                peerGone = true;
                for (auto& p : peers) p.gone = true;
                syncPeer();
                if (peerLeftNote.empty()) peerLeftNote = net.error().empty() ? "host disconnected" : net.error();
            }
        }
        if (peerGone && !peerGoneOk()) {
            if (fatal.empty()) fatal = !peerLeftNote.empty() ? peerLeftNote : "opponent disconnected";
            return;
        }

        if (phase == Pick && myReady && allPeersReady()) {
            phase = Battle;
            sentResult = false;
            myHalftime = myPastHalftime = false;
            myRes = {};
            for (auto& p : peers) {
                p.res = {};
                p.progress = 0;
                p.haveCd = p.halftime = p.pastHalftime = p.haveRot = false;
                p.cd = p.halftimeCd = 0.0f;
                p.rot = glm::vec3(0.0f);
            }
            syncPeer();
            startPending = true;
        }
        if (phase == Battle && myRes.have && allPeersDoneOrGone()) phase = Result;
        if (phase == Result && myRematch && allActivePeersRematch()) {
            if (isHost) {
                net.send("START_PICK\t1");
                enterPick(true);
            }
        }
    }

    bool allPeersReady() const {
        if (peers.empty()) return false;
        for (const auto& p : peers) if (!p.gone && !p.ready) return false;
        return true;
    }

    bool allPeersDoneOrGone() const {
        if (peers.empty()) return false;
        for (const auto& p : peers) if (!p.gone && !p.res.have) return false;
        return true;
    }

    bool allActivePeersRematch() const {
        int active = 0;
        for (const auto& p : peers) {
            if (!p.gone) {
                ++active;
                if (!p.rematch) return false;
            }
        }
        return active > 0;
    }

private:
    bool helloSent = false, gotHello = false;

    Peer* findPeer(int id) {
        for (auto& p : peers) if (p.id == id) return &p;
        if (peers.empty()) {
            peers.push_back({});
            peers[0].id = id;
            return &peers[0];
        }
        return nullptr;
    }

    void syncPeer() {
        if (!peers.empty()) peer = peers[0];
        else peer = {};
    }

    // after peer(s) are gone the match can only continue if nothing more is needed from them
    bool peerGoneOk() const {
        if (phase == Result) return true;
        if (phase == Battle) {
            if (peers.empty()) return false;
            for (const auto& p : peers) {
                if (p.gone && !p.res.have) return false;
            }
            for (const auto& p : peers) {
                if (!p.res.have) return false;
            }
            return true;
        }
        return false;
    }

    void resetAll() {
        phase = Lobby; myModel.clear(); myReady = myRematch = sentResult = false; myRes = {};
        myHalftime = myPastHalftime = false;
        peer = {}; peers.clear(); peerGone = false; startPending = false; fatal.clear(); lobbyError.clear();
        peerLeftNote.clear(); helloSent = gotHello = false;
    }

    void enterPick(bool rematch) {
        phase = Pick;
        for (auto& p : peers) {
            std::string keepName = p.name;
            int keepId = p.id;
            bool keepHost = p.isHost;
            p = {};
            p.name = keepName;
            p.id = keepId;
            p.isHost = keepHost;
        }
        syncPeer();
        myReady = myRematch = sentResult = false;
        myHalftime = myPastHalftime = false;
        myRes = {};
        (void)rematch;
        if (!myModel.empty()) {
            if (isHost) net.send("PICK\t0\t" + myModel);
            else net.send("PICK\t" + myModel);
        }
    }

    void handle(int senderId, const std::string& line) {
        if (line == "__CONNECT__") return;
        if (line == "__DISCONNECT__") {
            if (isHost) {
                if (phase == Lobby) {
                    peers.erase(std::remove_if(peers.begin(), peers.end(), [senderId](const Peer& p) {
                        return p.id == senderId;
                    }), peers.end());
                    net.send("LEAVE\t" + std::to_string(senderId));
                } else {
                    if (Peer* p = findPeer(senderId)) p->gone = true;
                    net.send("LEAVE\t" + std::to_string(senderId));
                    peerGone = true;
                    if (peerLeftNote.empty()) peerLeftNote = "opponent disconnected";
                }
            } else {
                peerGone = true;
                if (peerLeftNote.empty()) peerLeftNote = "host disconnected";
            }
            syncPeer();
            return;
        }

        auto f = split(line);
        if (f.empty()) return;
        const std::string& c = f[0];

        if (c == "HELLO" && f.size() >= 3) {
            if (isHost) {
                if (std::atoi(f[1].c_str()) != kProtocol) {
                    fatal = "opponent runs an incompatible version";
                    net.sendTo(senderId, "ERROR\tincompatible version");
                    return;
                }
                if (peers.size() >= (size_t)NetPeer::kMaxClients) {
                    net.sendTo(senderId, "ERROR\tlobby is full");
                    return;
                }
                Peer p;
                p.id = senderId;
                p.name = f[2].empty() ? ("Player " + std::to_string(senderId)) : f[2];
                peers.push_back(p);
                // Welcome to new player
                net.sendTo(senderId, "WELCOME\t" + std::to_string(senderId) + "\t" + std::to_string((int)goal) + "\t" + clean(myName));
                // Inform new player of all other existing peers
                for (const auto& other : peers) {
                    if (other.id != senderId) {
                        net.sendTo(senderId, "JOIN\t" + std::to_string(other.id) + "\t" + clean(other.name));
                        if (other.picked) net.sendTo(senderId, "PICK\t" + std::to_string(other.id) + "\t" + other.model);
                        if (other.ready) net.sendTo(senderId, "READY\t" + std::to_string(other.id) + "\t" + other.model);
                    }
                }
                // Inform existing clients of new player
                for (const auto& other : peers) {
                    if (other.id != senderId) {
                        net.sendTo(other.id, "JOIN\t" + std::to_string(senderId) + "\t" + clean(p.name));
                    }
                }
                gotHello = true;
                syncPeer();
                if (!manualStart) enterPick(false);
            }
        } else if (c == "WELCOME" && f.size() >= 4) {
            myId = std::atoi(f[1].c_str());
            goal = (Goal)std::atoi(f[2].c_str());
            Peer hp;
            hp.id = 0;
            hp.name = f[3].empty() ? "Host" : f[3];
            hp.isHost = true;
            peers.clear();
            peers.push_back(hp);
            gotHello = true;
            syncPeer();
            if (!manualStart) enterPick(false);
        } else if (c == "JOIN" && f.size() >= 3) {
            int id = std::atoi(f[1].c_str());
            if (!findPeer(id)) {
                Peer p;
                p.id = id;
                p.name = f[2];
                peers.push_back(p);
                syncPeer();
            }
        } else if (c == "LEAVE" && f.size() >= 2) {
            int id = std::atoi(f[1].c_str());
            if (phase == Lobby) {
                peers.erase(std::remove_if(peers.begin(), peers.end(), [id](const Peer& p) {
                    return p.id == id;
                }), peers.end());
            } else {
                if (Peer* p = findPeer(id)) p->gone = true;
                peerGone = true;
            }
            syncPeer();
        } else if (c == "GOAL" && f.size() >= 2) {
            goal = (Goal)std::atoi(f[1].c_str());
        } else if (c == "START_PICK") {
            enterPick(f.size() >= 2 && f[1] == "1");
        } else if (c == "PICK" && f.size() >= 2) {
            int slot = (f.size() >= 3) ? std::atoi(f[1].c_str()) : senderId;
            std::string model = (f.size() >= 3) ? f[2] : f[1];
            if (Peer* p = findPeer(slot)) {
                p->model = model; p->picked = true;
            }
            if (isHost && f.size() == 2) net.send("PICK\t" + std::to_string(slot) + "\t" + model);
            syncPeer();
        } else if (c == "READY" && f.size() >= 2) {
            int slot = (f.size() >= 3) ? std::atoi(f[1].c_str()) : senderId;
            std::string model = (f.size() >= 3) ? f[2] : f[1];
            if (Peer* p = findPeer(slot)) {
                p->model = model; p->picked = true; p->ready = true;
            }
            if (isHost && f.size() == 2) net.send("READY\t" + std::to_string(slot) + "\t" + model);
            syncPeer();
        } else if (c == "PROGRESS" && f.size() >= 2) {
            int slot = (f.size() >= 3) ? std::atoi(f[1].c_str()) : senderId;
            int pct = std::max(0, std::min(100, std::atoi((f.size() >= 3 ? f[2] : f[1]).c_str())));
            if (Peer* p = findPeer(slot)) p->progress = pct;
            if (isHost && f.size() == 2) net.send("PROGRESS\t" + std::to_string(slot) + "\t" + std::to_string(pct));
            syncPeer();
        } else if (c == "RESULT" && f.size() >= 4) {
            int slot = (f.size() >= 5) ? std::atoi(f[1].c_str()) : senderId;
            size_t off = (f.size() >= 5) ? 1 : 0;
            float cd = std::strtof(f[off + 1].c_str(), nullptr);
            if (!std::isfinite(cd)) cd = 0.0f;
            int tris = std::max(1, std::atoi(f[off + 2].c_str()));
            std::string model = f[off + 3];
            if (Peer* p = findPeer(slot)) {
                p->res = {true, cd, tris, model};
                p->progress = 100;
                p->cd = cd; p->haveCd = true;
                p->halftime = false;
            }
            if (isHost && f.size() == 4) {
                char b[64];
                std::snprintf(b, sizeof b, "%.9g", (double)cd);
                net.send("RESULT\t" + std::to_string(slot) + "\t" + b + "\t" + std::to_string(tris) + "\t" + model);
            }
            syncPeer();
        } else if (c == "CD" && f.size() >= 2) {
            int slot = (f.size() >= 3) ? std::atoi(f[1].c_str()) : senderId;
            float cd = std::strtof((f.size() >= 3 ? f[2] : f[1]).c_str(), nullptr);
            if (std::isfinite(cd)) {
                if (Peer* p = findPeer(slot)) {
                    if (!p->res.have) { p->cd = cd; p->haveCd = true; }
                }
                if (isHost && f.size() == 2) net.send("CD\t" + std::to_string(slot) + "\t" + fmtFloat(cd));
            }
            syncPeer();
        } else if (c == "HALF" && f.size() >= 2) {
            int slot = (f.size() >= 3) ? std::atoi(f[1].c_str()) : senderId;
            float cd = std::strtof((f.size() >= 3 ? f[2] : f[1]).c_str(), nullptr);
            if (!std::isfinite(cd)) cd = 0.0f;
            if (Peer* p = findPeer(slot)) {
                if (!p->res.have) {
                    p->halftime = true;
                    p->halftimeCd = p->cd = cd; p->haveCd = true;
                    p->progress = std::max(p->progress, 50);
                }
            }
            if (isHost && f.size() == 2) net.send("HALF\t" + std::to_string(slot) + "\t" + fmtFloat(cd));
            syncPeer();
        } else if (c == "ROT") {
            int slot = (f.size() >= 5) ? std::atoi(f[1].c_str()) : senderId;
            size_t off = (f.size() >= 5) ? 2 : 1;
            glm::vec3 r;
            if (parseRot(f, r, off)) {
                if (Peer* p = findPeer(slot)) {
                    p->rot = r; p->haveRot = true;
                }
                if (isHost && f.size() == 4) {
                    net.send("ROT\t" + std::to_string(slot) + "\t" + fmtFloat(r.x) + "\t" + fmtFloat(r.y) + "\t" + fmtFloat(r.z));
                }
            }
            syncPeer();
        } else if (c == "RESUME") {
            int slot = (f.size() >= 5) ? std::atoi(f[1].c_str()) : senderId;
            size_t off = (f.size() >= 5) ? 2 : 1;
            glm::vec3 r;
            if (parseRot(f, r, off)) {
                if (Peer* p = findPeer(slot)) {
                    p->rot = r; p->haveRot = true;
                    p->halftime = false;
                    p->pastHalftime = true;
                }
                if (isHost && f.size() == 4) {
                    net.send("RESUME\t" + std::to_string(slot) + "\t" + fmtFloat(r.x) + "\t" + fmtFloat(r.y) + "\t" + fmtFloat(r.z));
                }
            }
            syncPeer();
        } else if (c == "REMATCH") {
            int slot = (f.size() >= 2) ? std::atoi(f[1].c_str()) : senderId;
            if (Peer* p = findPeer(slot)) p->rematch = true;
            if (isHost && f.size() == 1) net.send("REMATCH\t" + std::to_string(slot));
            syncPeer();
        } else if (c == "BYE") {
            if (isHost) {
                if (Peer* p = findPeer(senderId)) p->gone = true;
                net.send("LEAVE\t" + std::to_string(senderId));
            }
            peerGone = true;
            peerLeftNote = "opponent left the match";
            syncPeer();
        } else if (c == "ERROR" && f.size() >= 2) {
            fatal = f[1];
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
// halftime: mpAfterStep stops counting at kHalfSteps and the UI switches to APP_SCREEN_MP_HALFTIME (main stops the
// solver there like behind any menu). "Resume" queues a request; main then does stopSolver -> rotation = rot ->
// buildModel -> mpSimResume -> startSolver. The step counter continues from kHalfSteps.
bool mpTakeResume(glm::vec3& rotationDeg);         // main: true once when the player resumes from halftime
void mpSimResume();                                // main: after buildModel, just before the solver restarts
void mpShutdown();                                 // on exit: close sockets
