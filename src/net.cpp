#include "net.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    #include <vector>
    using ssize_t = int;
    using nfds_t = ULONG;
    #ifndef MSG_NOSIGNAL
        #define MSG_NOSIGNAL 0
    #endif
    static int netErr() { return WSAGetLastError(); }
    static bool errWouldBlock(int e) { return e == WSAEWOULDBLOCK; }
    static bool errIntr(int e) { return e == WSAEINTR; }
    static bool errInProgress(int e) { return e == WSAEWOULDBLOCK; }
    static std::string errStr(int e) {
        char* m = nullptr;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, (DWORD)e, 0, (LPSTR)&m, 0, nullptr);
        std::string r = m ? m : "error " + std::to_string(e);
        if (m) LocalFree(m);
        while (!r.empty() && (r.back() == '\n' || r.back() == '\r' || r.back() == ' ')) r.pop_back();
        return r;
    }
    static void closeSock(int fd) { closesocket((SOCKET)fd); }
    static int pollSock(pollfd* p, nfds_t n, int ms) { return WSAPoll(p, n, ms); }
    static void setNonBlocking(int fd) { u_long on = 1; ioctlsocket((SOCKET)fd, FIONBIO, &on); }
    static void netInit() {
        static std::once_flag f;
        std::call_once(f, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
    }
#else
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
    static int netErr() { return errno; }
    static bool errWouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
    static bool errIntr(int e) { return e == EINTR; }
    static bool errInProgress(int e) { return e == EINPROGRESS; }
    static std::string errStr(int e) { return strerror(e); }
    static void closeSock(int fd) { ::close(fd); }
    static int pollSock(pollfd* p, nfds_t n, int ms) { return ::poll(p, n, ms); }
    static void setNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
    static void netInit() {}
#endif

static constexpr size_t kMaxLine = 4096;

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
    outbox.push_back({0, line});
}

void NetPeer::sendTo(int clientId, const std::string& line) {
    std::lock_guard<std::mutex> l(mu);
    outbox.push_back({clientId, line});
}

bool NetPeer::pop(std::string& line) {
    int cid = 0;
    return pop(cid, line);
}

bool NetPeer::pop(int& clientId, std::string& line) {
    std::lock_guard<std::mutex> l(mu);
    if (inbox.empty()) return false;
    clientId = inbox.front().clientId;
    line = std::move(inbox.front().line);
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
    hosting = false;
    numClients = 0;
}

void NetPeer::host(int port, int maxClients) {
    close();
    hosting = true;
    st = NetState::Listening;
    th = std::thread([this, port, maxClients] { runHost(port, maxClients); });
}

void NetPeer::join(const std::string& address, int port) {
    close();
    hosting = false;
    st = NetState::Connecting;
    th = std::thread([this, address, port] { runJoin(address, port); });
}

void NetPeer::runHost(int port, int maxClients) {
    netInit();
    int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return fail(NetState::Failed, std::string("socket: ") + errStr(netErr()));
    int one = 1;
#ifndef _WIN32 // on Windows SO_REUSEADDR lets another process steal the port
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (::bind(ls, (sockaddr*)&a, sizeof a) < 0 || ::listen(ls, 8) < 0) {
        std::string why = std::string("can't listen on port ") + std::to_string(port) + ": " + errStr(netErr());
        closeSock(ls);
        return fail(NetState::Failed, why);
    }
    setNonBlocking(ls);

    struct ClientConn {
        int id = 0;
        int fd = -1;
        std::string rbuf;
        std::string wbuf;
    };
    std::vector<ClientConn> clients;
    int nextClientId = 1;

    while (!stop) {
        // 1. Drain outbox
        {
            std::lock_guard<std::mutex> l(mu);
            while (!outbox.empty()) {
                auto item = std::move(outbox.front());
                outbox.pop_front();
                for (auto& c : clients) {
                    if (item.targetId == 0 || item.targetId == c.id) {
                        c.wbuf += item.line;
                        c.wbuf += '\n';
                    }
                }
            }
        }

        // 2. Build pollfd array
        std::vector<pollfd> pfds;
        bool canAccept = (int)clients.size() < maxClients;
        if (canAccept) {
            pfds.push_back({ls, POLLIN, 0});
        }
        for (const auto& c : clients) {
            short ev = POLLIN;
            if (!c.wbuf.empty()) ev |= POLLOUT;
            pfds.push_back({c.fd, ev, 0});
        }

        int pr = pollSock(pfds.data(), (nfds_t)pfds.size(), 20);
        if (pr < 0 && !errIntr(netErr())) {
            fail(NetState::Failed, "network error");
            break;
        }

        // 3. Accept new connections
        size_t clientStartIdx = 0;
        if (canAccept) {
            clientStartIdx = 1;
            if (pr > 0 && (pfds[0].revents & POLLIN)) {
                int cfd = ::accept(ls, nullptr, nullptr);
                if (cfd >= 0) {
                    setNonBlocking(cfd);
                    int flag = 1;
#ifdef __APPLE__
                    setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&flag, sizeof flag);
#endif
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof flag);
                    int cid = nextClientId++;
                    clients.push_back({cid, cfd, "", ""});
                    {
                        std::lock_guard<std::mutex> l(mu);
                        inbox.push_back({cid, "__CONNECT__"});
                        numClients = (int)clients.size();
                    }
                    st = NetState::Connected;
                }
            }
        }

        // 4. Handle client I/O
        char tmp[2048];
        std::vector<int> deadClientIds;
        for (size_t i = 0; i < clients.size(); ++i) {
            size_t pidx = clientStartIdx + i;
            if (pidx >= pfds.size()) break;
            auto& c = clients[i];
            const auto& pfd = pfds[pidx];

            if (pfd.revents & POLLOUT) {
#ifdef __APPLE__
                ssize_t n = ::send(c.fd, c.wbuf.data(), c.wbuf.size(), 0);
#else
                ssize_t n = ::send(c.fd, c.wbuf.data(), c.wbuf.size(), MSG_NOSIGNAL);
#endif
                if (n > 0) {
                    c.wbuf.erase(0, (size_t)n);
                } else if (n < 0 && !errWouldBlock(netErr()) && !errIntr(netErr())) {
                    deadClientIds.push_back(c.id);
                    continue;
                }
            }

            if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t n = ::recv(c.fd, tmp, sizeof tmp, 0);
                if (n <= 0) {
                    if (n == 0 || (!errWouldBlock(netErr()) && !errIntr(netErr()))) {
                        deadClientIds.push_back(c.id);
                        continue;
                    }
                } else {
                    c.rbuf.append(tmp, (size_t)n);
                    size_t pos;
                    while ((pos = c.rbuf.find('\n')) != std::string::npos) {
                        std::string line = c.rbuf.substr(0, pos);
                        c.rbuf.erase(0, pos + 1);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (!line.empty()) {
                            std::lock_guard<std::mutex> l(mu);
                            inbox.push_back({c.id, std::move(line)});
                        }
                    }
                    if (c.rbuf.size() > kMaxLine) {
                        deadClientIds.push_back(c.id);
                        continue;
                    }
                }
            }
        }

        // 5. Clean up disconnected clients
        if (!deadClientIds.empty()) {
            for (int deadId : deadClientIds) {
                auto it = std::find_if(clients.begin(), clients.end(), [deadId](const ClientConn& cc) {
                    return cc.id == deadId;
                });
                if (it != clients.end()) {
                    closeSock(it->fd);
                    clients.erase(it);
                    std::lock_guard<std::mutex> l(mu);
                    inbox.push_back({deadId, "__DISCONNECT__"});
                }
            }
            numClients = (int)clients.size();
            st = clients.empty() ? NetState::Listening : NetState::Connected;
        }
    }

    for (auto& c : clients) {
        if (!c.wbuf.empty()) {
#ifdef __APPLE__
            ::send(c.fd, c.wbuf.data(), c.wbuf.size(), 0);
#else
            ::send(c.fd, c.wbuf.data(), c.wbuf.size(), MSG_NOSIGNAL);
#endif
        }
        closeSock(c.fd);
    }
    closeSock(ls);
    clients.clear();
    numClients = 0;
}

void NetPeer::runJoin(std::string address, int port) {
    netInit();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (gai != 0 || !res) return fail(NetState::Failed, "bad address '" + address + "'");
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return fail(NetState::Failed, std::string("socket: ") + errStr(netErr())); }
    setNonBlocking(fd);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && !errInProgress(netErr())) {
        std::string why = std::string("connect failed: ") + errStr(netErr());
        closeSock(fd);
        return fail(NetState::Failed, why);
    }
    // wait up to ~6 s in short slices so close() can cancel
    bool ok = rc == 0;
    for (int i = 0; !ok && i < 60 && !stop; i++) {
        pollfd p{fd, POLLOUT, 0};
        if (pollSock(&p, 1, 100) > 0) {
            int e = 0; socklen_t len = sizeof e;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&e, &len);
            if (e != 0) {
                closeSock(fd);
                return fail(NetState::Failed, std::string("connect failed: ") + errStr(e));
            }
            ok = true;
        }
    }
    if (stop) { closeSock(fd); return; }
    if (!ok) { closeSock(fd); return fail(NetState::Failed, "connection timed out"); }
    ioLoop(fd);
}

void NetPeer::ioLoop(int fd) {
    setNonBlocking(fd);
    int one = 1;
#ifdef __APPLE__
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&one, sizeof one);
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    st = NetState::Connected;
    numClients = 1;

    std::string rbuf, wbuf;
    char tmp[2048];
    auto drainOutbox = [&] {
        std::lock_guard<std::mutex> l(mu);
        while (!outbox.empty()) {
            wbuf += outbox.front().line;
            wbuf += '\n';
            outbox.pop_front();
        }
    };
    while (!stop) {
        drainOutbox();
        pollfd p{fd, (short)(POLLIN | (wbuf.empty() ? 0 : POLLOUT)), 0};
        int pr = pollSock(&p, 1, 20);
        if (pr < 0 && !errIntr(netErr())) { fail(NetState::Failed, "network error"); break; }
        if (pr > 0 && (p.revents & POLLOUT)) {
#ifdef __APPLE__
            ssize_t n = ::send(fd, wbuf.data(), wbuf.size(), 0);
#else
            ssize_t n = ::send(fd, wbuf.data(), wbuf.size(), MSG_NOSIGNAL);
#endif
            if (n > 0) wbuf.erase(0, (size_t)n);
            else if (n < 0 && !errWouldBlock(netErr()) && !errIntr(netErr())) { fail(NetState::Failed, "connection lost"); break; }
        }
        if (pr > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
            if (n == 0) { fail(NetState::Closed, "opponent disconnected"); break; }
            if (n < 0) {
                if (!errWouldBlock(netErr()) && !errIntr(netErr())) { fail(NetState::Failed, "connection lost"); break; }
            } else {
                rbuf.append(tmp, (size_t)n);
                std::lock_guard<std::mutex> l(mu);
                size_t pos;
                while ((pos = rbuf.find('\n')) != std::string::npos) {
                    std::string line = rbuf.substr(0, pos);
                    rbuf.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) inbox.push_back({0, std::move(line)});
                }
            }
            if (rbuf.size() > kMaxLine) { fail(NetState::Failed, "protocol error (line too long)"); break; }
        }
    }
    // best-effort flush of a final message (e.g. BYE) when we are the one closing
    if (st == NetState::Connected) {
        drainOutbox();
#ifdef __APPLE__
        if (!wbuf.empty()) ::send(fd, wbuf.data(), wbuf.size(), 0);
#else
        if (!wbuf.empty()) ::send(fd, wbuf.data(), wbuf.size(), MSG_NOSIGNAL);
#endif
    }
    closeSock(fd);
    numClients = 0;
}

std::vector<std::string> NetPeer::localAddresses() {
    std::vector<std::string> out;
#ifdef _WIN32
    ULONG len = 15000;
    std::vector<char> buf(len);
    auto* addrs = (IP_ADAPTER_ADDRESSES*)buf.data();
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, addrs, &len);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(len);
        addrs = (IP_ADAPTER_ADDRESSES*)buf.data();
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, addrs, &len);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof name - 1, nullptr, nullptr);
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((sockaddr_in*)u->Address.lpSockaddr)->sin_addr, ip, sizeof ip);
            out.push_back(std::string(ip) + " (" + name + ")");
        }
    }
    return out;
#else
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
#endif
}
