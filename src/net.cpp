#include "net.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static constexpr size_t kMaxLine = 4096;

static void setNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

void NetPeer::fail(NetState s, const std::string& why) {
    { std::lock_guard<std::mutex> l(mu); err = why; }
    st = s;
}

std::string NetPeer::error() const {
    std::lock_guard<std::mutex> l(mu);
    return err;
}

void NetPeer::send(const std::string& line) {
    std::lock_guard<std::mutex> l(mu);
    outbox.push_back(line);
}

bool NetPeer::pop(std::string& line) {
    std::lock_guard<std::mutex> l(mu);
    if (inbox.empty()) return false;
    line = std::move(inbox.front());
    inbox.pop_front();
    return true;
}

void NetPeer::close() {
    stop = true;
    if (th.joinable()) th.join();
    stop = false;
    std::lock_guard<std::mutex> l(mu);
    inbox.clear(); outbox.clear(); err.clear();
    st = NetState::Idle;
}

void NetPeer::host(int port) {
    close();
    st = NetState::Listening;
    th = std::thread([this, port] { runHost(port); });
}

void NetPeer::join(const std::string& address, int port) {
    close();
    st = NetState::Connecting;
    th = std::thread([this, address, port] { runJoin(address, port); });
}

void NetPeer::runHost(int port) {
    int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return fail(NetState::Failed, std::string("socket: ") + strerror(errno));
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (::bind(ls, (sockaddr*)&a, sizeof a) < 0 || ::listen(ls, 1) < 0) {
        std::string why = std::string("can't listen on port ") + std::to_string(port) + ": " + strerror(errno);
        ::close(ls);
        return fail(NetState::Failed, why);
    }
    setNonBlocking(ls);
    int fd = -1;
    while (!stop) {
        pollfd p{ls, POLLIN, 0};
        if (::poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
            fd = ::accept(ls, nullptr, nullptr);
            if (fd >= 0) break;
        }
    }
    ::close(ls);
    if (fd < 0) return; // cancelled
    ioLoop(fd);
}

void NetPeer::runJoin(std::string address, int port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (gai != 0 || !res) return fail(NetState::Failed, "bad address '" + address + "'");
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return fail(NetState::Failed, std::string("socket: ") + strerror(errno)); }
    setNonBlocking(fd);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) {
        std::string why = std::string("connect failed: ") + strerror(errno);
        ::close(fd);
        return fail(NetState::Failed, why);
    }
    // wait up to ~6 s in short slices so close() can cancel
    bool ok = rc == 0;
    for (int i = 0; !ok && i < 60 && !stop; i++) {
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, 100) > 0) {
            int e = 0; socklen_t len = sizeof e;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &len);
            if (e != 0) {
                ::close(fd);
                return fail(NetState::Failed, std::string("connect failed: ") + strerror(e));
            }
            ok = true;
        }
    }
    if (stop) { ::close(fd); return; }
    if (!ok) { ::close(fd); return fail(NetState::Failed, "connection timed out"); }
    ioLoop(fd);
}

void NetPeer::ioLoop(int fd) {
    setNonBlocking(fd);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    st = NetState::Connected;

    std::string rbuf, wbuf;
    char tmp[2048];
    auto drainOutbox = [&] {
        std::lock_guard<std::mutex> l(mu);
        while (!outbox.empty()) { wbuf += outbox.front(); wbuf += '\n'; outbox.pop_front(); }
    };
    while (!stop) {
        drainOutbox();
        pollfd p{fd, (short)(POLLIN | (wbuf.empty() ? 0 : POLLOUT)), 0};
        int pr = ::poll(&p, 1, 20);
        if (pr < 0 && errno != EINTR) { fail(NetState::Failed, "network error"); break; }
        if (pr > 0 && (p.revents & POLLOUT)) {
            ssize_t n = ::send(fd, wbuf.data(), wbuf.size(), 0);
            if (n > 0) wbuf.erase(0, (size_t)n);
            else if (n < 0 && errno != EAGAIN && errno != EINTR) { fail(NetState::Failed, "connection lost"); break; }
        }
        if (pr > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
            if (n == 0) { fail(NetState::Closed, "opponent disconnected"); break; }
            if (n < 0) {
                if (errno != EAGAIN && errno != EINTR) { fail(NetState::Failed, "connection lost"); break; }
            } else {
                rbuf.append(tmp, (size_t)n);
                std::lock_guard<std::mutex> l(mu);
                size_t pos;
                while ((pos = rbuf.find('\n')) != std::string::npos) {
                    std::string line = rbuf.substr(0, pos);
                    rbuf.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) inbox.push_back(std::move(line));
                }
            }
            if (rbuf.size() > kMaxLine) { fail(NetState::Failed, "protocol error (line too long)"); break; }
        }
    }
    // best-effort flush of a final message (e.g. BYE) when we are the one closing
    if (st == NetState::Connected) {
        drainOutbox();
        if (!wbuf.empty()) ::send(fd, wbuf.data(), wbuf.size(), 0);
    }
    ::close(fd);
}

std::vector<std::string> NetPeer::localAddresses() {
    std::vector<std::string> out;
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return out;
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || (p->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((sockaddr_in*)p->ifa_addr)->sin_addr, buf, sizeof buf);
        out.push_back(std::string(buf) + " (" + p->ifa_name + ")");
    }
    freeifaddrs(ifa);
    return out;
}
