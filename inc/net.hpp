#pragma once
// Tiny line-based TCP peer (POSIX sockets, one background thread). The render thread never blocks:
// send() only queues, pop() only dequeues, state()/error() are cheap reads.
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class NetState { Idle, Listening, Connecting, Connected, Closed, Failed };

class NetPeer {
public:
    NetPeer() = default;
    ~NetPeer() { close(); }
    NetPeer(const NetPeer&) = delete;
    NetPeer& operator=(const NetPeer&) = delete;

    void host(int port);                                  // listen, accept one client
    void join(const std::string& address, int port);      // connect (IPv4 literal or hostname)
    void close();                                         // stop + join the thread, reset to Idle

    void send(const std::string& line);                   // queue one line (no trailing newline)
    bool pop(std::string& line);                          // next received line, false if none

    NetState state() const { return st.load(); }
    std::string error() const;                            // reason when Failed/Closed
    static std::vector<std::string> localAddresses();     // non-loopback IPv4 addresses

private:
    void runHost(int port);
    void runJoin(std::string address, int port);
    void ioLoop(int fd);
    void fail(NetState s, const std::string& why);

    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<NetState> st{NetState::Idle};
    mutable std::mutex mu;
    std::deque<std::string> inbox, outbox;
    std::string err;
};
