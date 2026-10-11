// Headless loopback test of mp::Match (no raylib/imgui). Two matches talk over 127.0.0.1 in one process.
// Build + run (from the repo root):
//   g++ -std=c++23 -I inc -I <glm-src> tests/mp_match_test.cpp src/net.cpp -o /tmp/mp_match_test -pthread && /tmp/mp_match_test
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include "multiplayer.hpp"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool pumpUntil(mp::Match& a, mp::Match& b, const std::function<bool()>& done, int ms = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        a.update();
        b.update();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

static int port = 47770;

// lobby -> pick -> both ready -> battle
static bool startBattle(mp::Match& h, mp::Match& j) {
    h.myName = "Host"; j.myName = "Joiner";
    ++port;
    h.hostGame(port);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    j.joinGame("127.0.0.1", port);
    if (!pumpUntil(h, j, [&] { return h.phase == mp::Match::Pick && j.phase == mp::Match::Pick; })) return false;
    h.setPick("car.obj"); j.setPick("wing.obj");
    h.setReady(); j.setReady();
    if (!pumpUntil(h, j, [&] { return h.phase == mp::Match::Battle && j.phase == mp::Match::Battle; })) return false;
    return h.takeStart() && j.takeStart();
}

static void testFullFlow() {
    std::printf("full flow with halftime\n");
    mp::Match h, j;
    CHECK(startBattle(h, j));
    CHECK(h.peer.name == "Joiner" && j.peer.name == "Host");
    CHECK(!j.peer.haveCd && !j.peer.halftime && !j.peer.haveRot);

    h.sendRotation({10, -20, 30});
    h.sendProgress(30);
    h.sendCd(0.8125f);
    CHECK(pumpUntil(h, j, [&] { return j.peer.haveCd && j.peer.haveRot && j.peer.progress == 30; }));
    CHECK(j.peer.cd == 0.8125f);
    CHECK(j.peer.rot == glm::vec3(10, -20, 30));

    // host reaches halftime first, joiner keeps racing
    h.enterHalftime(0.75f);
    CHECK(h.myHalftime);
    CHECK(pumpUntil(h, j, [&] { return j.peer.halftime; }));
    CHECK(j.peer.halftimeCd == 0.75f && j.peer.cd == 0.75f && j.peer.progress >= 50);
    h.enterHalftime(0.1f); // ignored: already at halftime
    // live rotation while at halftime
    h.sendRotation({45, 0, -90});
    CHECK(pumpUntil(h, j, [&] { return j.peer.rot == glm::vec3(45, 0, -90); }));
    // ROT values are clamped and junk is ignored
    h.net.send("ROT\t999\tnan\t1");
    h.net.send("ROT\t500\t-500\t1");
    CHECK(pumpUntil(h, j, [&] { return j.peer.rot == glm::vec3(180, -180, 1); }));

    j.sendCd(1.25f);
    CHECK(pumpUntil(h, j, [&] { return h.peer.haveCd && h.peer.cd == 1.25f; }));
    CHECK(!h.peer.halftime);

    h.leaveHalftime({5, 6, 7});
    CHECK(!h.myHalftime && h.myPastHalftime);
    CHECK(pumpUntil(h, j, [&] { return !j.peer.halftime && j.peer.pastHalftime; }));
    CHECK(j.peer.rot == glm::vec3(5, 6, 7));
    h.enterHalftime(0.5f); // halftime only once
    CHECK(!h.myHalftime);

    // joiner reaches halftime later than the host - nobody waited for anybody
    j.enterHalftime(1.0f);
    CHECK(pumpUntil(h, j, [&] { return h.peer.halftime; }));
    j.leaveHalftime({0, 0, 0});
    CHECK(pumpUntil(h, j, [&] { return h.peer.pastHalftime && !h.peer.halftime; }));

    h.finished(0.9f, 1000);
    j.finished(1.8f, 1000);
    CHECK(pumpUntil(h, j, [&] { return h.phase == mp::Match::Result && j.phase == mp::Match::Result; }));
    CHECK(mp::verdict(h.myRes, h.peer.res) == mp::Verdict::Lose);
    CHECK(mp::verdict(j.myRes, j.peer.res) == mp::Verdict::Win);
    CHECK(h.peer.cd == 1.8f); // RESULT also updates the live Cd

    // rematch resets halftime state on both sides
    h.requestRematch(); j.requestRematch();
    CHECK(pumpUntil(h, j, [&] { return h.phase == mp::Match::Pick && j.phase == mp::Match::Pick; }));
    CHECK(!h.myHalftime && !h.myPastHalftime && !h.peer.halftime && !h.peer.pastHalftime && !h.peer.haveCd);
    h.setReady(); j.setReady();
    CHECK(pumpUntil(h, j, [&] { return h.phase == mp::Match::Battle && j.phase == mp::Match::Battle; }));
    CHECK(!h.myHalftime && !h.myPastHalftime && !j.peer.halftime && !j.peer.haveRot);
    h.leave(); j.leave();
}

static void testHalftimeWhileOpponentFinished() {
    std::printf("me at halftime, opponent already finished and gone\n");
    mp::Match h, j;
    CHECK(startBattle(h, j));
    j.finished(1.0f, 500);
    CHECK(pumpUntil(h, j, [&] { return h.peer.res.have; }));
    h.enterHalftime(0.7f);
    j.leave(); // finished opponent leaves: I can still complete
    CHECK(pumpUntil(h, j, [&] { return h.peerGone; }));
    CHECK(h.fatal.empty());
    h.leaveHalftime({0, 90, 0});
    h.finished(0.9f, 400);
    CHECK(pumpUntil(h, j, [&] { return h.phase == mp::Match::Result; }));
    CHECK(h.fatal.empty());
    h.leave();
}

static void testDisconnectAtHalftime() {
    std::printf("opponent leaves (unfinished) while I'm at halftime\n");
    mp::Match h, j;
    CHECK(startBattle(h, j));
    h.enterHalftime(0.7f);
    CHECK(pumpUntil(h, j, [&] { return j.peer.halftime; }));
    j.leave();
    CHECK(pumpUntil(h, j, [&] { return !h.fatal.empty(); }));
    h.leave();

    std::printf("halftime player forfeits (peer sees fatal)\n");
    mp::Match a, b;
    CHECK(startBattle(a, b));
    a.enterHalftime(0.7f);
    CHECK(pumpUntil(a, b, [&] { return b.peer.halftime; }));
    a.leave(); // Forfeit button
    CHECK(pumpUntil(a, b, [&] { return !b.fatal.empty(); }));
    b.leave();
}

static void testParseOnly() {
    std::printf("message parsing edge cases\n");
    glm::vec3 r(1.0f);
    CHECK(!mp::parseRot(mp::split("ROT\t1\t2"), r) && r == glm::vec3(1.0f));
    CHECK(mp::parseRot(mp::split("RESUME\t-1.5\t2\t3"), r) && r == glm::vec3(-1.5f, 2, 3));
    CHECK(mp::fmtFloat(NAN) == "0");
}

int main() {
    testParseOnly();
    testFullFlow();
    testHalftimeWhileOpponentFinished();
    testDisconnectAtHalftime();
    std::printf(failures ? "%d FAILURE(S)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
