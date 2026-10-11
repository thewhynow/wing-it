#pragma once
// Line-based TCP peer (POSIX sockets, background thread). The render thread never blocks:
// send() only queues, pop() only dequeues, state()/error() are cheap reads.
// When hosting, supports multiple clients concurrently (up to kMaxClients).
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class NetState { Idle, Listening, Connecting, Connected, Closed, Failed };

struct NetMessage {
    int clientId = 0; // 0 for host/server or default, 1..kMaxClients for clients
    std::string line;
};

class NetPeer {
public:
    static constexpr int kMaxClients = 3; // host + 3 clients = 4 players max

    NetPeer() = default;
    ~NetPeer() { close(); }
    NetPeer(const NetPeer&) = delete;
    NetPeer& operator=(const NetPeer&) = delete;

    void host(int port, int maxClients = kMaxClients);   // listen, accept up to maxClients
    void join(const std::string& address, int port);      // connect (IPv4 literal or hostname)
    void close();                                         // stop + join the thread, reset to Idle

    void send(const std::string& line);                   // broadcast to all clients (host) or send to host (client)
    void sendTo(int clientId, const std::string& line);   // send to a specific client (host only)
    bool pop(std::string& line);                          // next received line
    bool pop(int& clientId, std::string& line);           // next received line with sender ID

    NetState state() const { return st.load(); }
    std::string error() const;                            // reason when Failed/Closed
    static std::vector<std::string> localAddresses();     // non-loopback IPv4 addresses
    int clientCount() const { return numClients.load(); }
    bool isHost() const { return hosting.load(); }

private:
    struct OutMsg { int targetId = 0; std::string line; }; // targetId 0 = broadcast

    void runHost(int port, int maxClients);
    void runJoin(std::string address, int port);
    void ioLoop(int fd);
    void fail(NetState s, const std::string& why);

    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<bool> hosting{false};
    std::atomic<int> numClients{0};
    std::atomic<NetState> st{NetState::Idle};
    mutable std::mutex mu;
    std::deque<NetMessage> inbox;
    std::deque<OutMsg> outbox;
    std::string err;
};
