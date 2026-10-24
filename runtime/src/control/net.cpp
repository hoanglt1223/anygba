#include "control/net.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace anygba::control::net {

namespace {

#if defined(_WIN32)
using Native = SOCKET;
constexpr Native kInvalid = INVALID_SOCKET;
int last_error() { return WSAGetLastError(); }
bool would_block(int e) { return e == WSAEWOULDBLOCK; }
#else
using Native = int;
constexpr Native kInvalid = -1;
int last_error() { return errno; }
bool would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN; }
#endif

Native native(const Socket& s) { return static_cast<Native>(s.fd()); }

void startup() {
#if defined(_WIN32)
    static std::once_flag once;
    static int result = 0;
    std::call_once(once, [] {
        WSADATA data;
        result = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (result != 0) throw NetError("WSAStartup failed: " + std::to_string(result));
#endif
}

void set_nonblocking(Native s) {
#if defined(_WIN32)
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

void set_nodelay(Native s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

Socket make_socket() {
    startup();
    const Native s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalid) throw NetError("cannot create a socket: error " + std::to_string(last_error()));
    return Socket(static_cast<std::intptr_t>(s));
}

sockaddr_in loopback(std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

// Waits for readability (or writability) of one socket. Returns false on timeout.
bool wait_one(Native s, bool write, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int n = ::select(static_cast<int>(s) + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr,
                           timeout_ms < 0 ? nullptr : &tv);
    return n > 0;
}

}  // namespace

Socket& Socket::operator=(Socket&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

void Socket::close() {
    if (fd_ == -1) return;
#if defined(_WIN32)
    ::shutdown(static_cast<Native>(fd_), SD_BOTH);
    ::closesocket(static_cast<Native>(fd_));
#else
    ::shutdown(static_cast<Native>(fd_), SHUT_RDWR);
    ::close(static_cast<Native>(fd_));
#endif
    fd_ = -1;
}

bool is_loopback_host(std::string_view host) { return host == "127.0.0.1" || host == "localhost"; }

std::pair<Socket, std::uint16_t> listen_loopback(std::string_view host, std::uint16_t port) {
    if (!is_loopback_host(host)) {
        throw NetError("the control server only listens on 127.0.0.1, not '" + std::string(host) + "'");
    }
    Socket s = make_socket();
#if defined(_WIN32)
    // Another program cannot take over the port while it is ours.
    int on = 1;
    setsockopt(native(s), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
#endif
    sockaddr_in a = loopback(port);
    if (::bind(native(s), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        throw NetError("cannot listen on 127.0.0.1:" + std::to_string(port) + ": error " + std::to_string(last_error()));
    }
    if (::listen(native(s), 4) != 0) {
        throw NetError("listen failed: error " + std::to_string(last_error()));
    }
    socklen_t len = sizeof a;
    ::getsockname(native(s), reinterpret_cast<sockaddr*>(&a), &len);
    set_nonblocking(native(s));
    return {std::move(s), ntohs(a.sin_port)};
}

std::optional<Socket> accept(Socket& listener) {
    sockaddr_in peer{};
    socklen_t len = sizeof peer;
    const Native c = ::accept(native(listener), reinterpret_cast<sockaddr*>(&peer), &len);
    if (c == kInvalid) return std::nullopt;
    Socket s(static_cast<std::intptr_t>(c));
    // Bound to 127.0.0.1 this cannot happen; checked anyway so a bind change can never expose it.
    if (peer.sin_family != AF_INET || (ntohl(peer.sin_addr.s_addr) >> 24) != 127) {
        return std::nullopt;
    }
    set_nonblocking(c);
    set_nodelay(c);
    return s;
}

std::vector<std::size_t> wait_readable(const std::vector<const Socket*>& sockets, int timeout_ms) {
    std::vector<std::size_t> ready;
    fd_set set;
    FD_ZERO(&set);
    Native top = 0;
    bool any = false;
    for (const Socket* s : sockets) {
        if (!s->valid()) continue;
        FD_SET(native(*s), &set);
        if (native(*s) > top) top = native(*s);
        any = true;
    }
    if (!any) {
        if (timeout_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return ready;
    }
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (::select(static_cast<int>(top) + 1, &set, nullptr, nullptr, &tv) <= 0) return ready;
    for (std::size_t i = 0; i < sockets.size(); ++i) {
        if (sockets[i]->valid() && FD_ISSET(native(*sockets[i]), &set)) ready.push_back(i);
    }
    return ready;
}

bool read_available(Socket& s, std::string& out) {
    char buf[16384];
    for (;;) {
        const auto n = ::recv(native(s), buf, sizeof buf, 0);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            if (static_cast<std::size_t>(n) < sizeof buf) return true;
            continue;
        }
        if (n == 0) return false;
        return would_block(last_error());
    }
}

bool send_all(Socket& s, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int chunk = static_cast<int>(std::min<std::size_t>(data.size() - sent, 1 << 20));
        const auto n = ::send(native(s), data.data() + sent, chunk, 0);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && would_block(last_error())) {
            if (!wait_one(native(s), true, 30000)) return false;  // the reader stopped reading
            continue;
        }
        return false;
    }
    return true;
}

Socket connect_loopback(std::uint16_t port) {
    Socket s = make_socket();
    sockaddr_in a = loopback(port);
    if (::connect(native(s), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        throw NetError("cannot connect to 127.0.0.1:" + std::to_string(port) + ": error " + std::to_string(last_error()));
    }
    set_nodelay(native(s));
    return s;
}

bool read_line(Socket& s, std::string& buffer, std::string& line, int timeout_ms) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        const auto nl = buffer.find('\n');
        if (nl != std::string::npos) {
            line.assign(buffer, 0, nl);
            buffer.erase(0, nl + 1);
            return true;
        }
        int wait = -1;
        if (timeout_ms >= 0) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (spent >= timeout_ms) return false;
            wait = static_cast<int>(timeout_ms - spent);
        }
        if (!wait_one(native(s), false, wait)) {
            if (timeout_ms >= 0) return false;
            continue;
        }
        char buf[65536];
        const auto n = ::recv(native(s), buf, sizeof buf, 0);
        if (n <= 0) throw NetError("the game closed the connection");
        buffer.append(buf, static_cast<std::size_t>(n));
    }
}

}  // namespace anygba::control::net
