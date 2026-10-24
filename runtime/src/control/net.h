#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Minimal IPv4 TCP for the control channel: a loopback-only listener, non-blocking accept
// and reads, and a blocking client. Winsock on Windows, BSD sockets elsewhere.
namespace anygba::control::net {

class NetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Socket {
public:
    Socket() = default;
    explicit Socket(std::intptr_t fd) : fd_(fd) {}
    ~Socket() { close(); }
    Socket(Socket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Socket& operator=(Socket&& o) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const { return fd_ != -1; }
    std::intptr_t fd() const { return fd_; }
    void close();

private:
    std::intptr_t fd_ = -1;
};

// True for the host names the control server accepts: 127.0.0.1 and localhost.
bool is_loopback_host(std::string_view host);

// Listens on 127.0.0.1:`port` (0 = any free port). Throws NetError for any other host or when
// the port is taken. Returns the socket and the bound port.
std::pair<Socket, std::uint16_t> listen_loopback(std::string_view host, std::uint16_t port);

// A pending connection, or nullopt. Connections whose peer is not a loopback address are
// closed here and never returned.
std::optional<Socket> accept(Socket& listener);

// Waits until one of `sockets` is readable or `timeout_ms` passes (0 = just check). Returns the
// indexes of the readable sockets.
std::vector<std::size_t> wait_readable(const std::vector<const Socket*>& sockets, int timeout_ms);

// Non-blocking read of what is available: appends to `out`. Returns false when the peer closed
// the connection or it failed.
bool read_available(Socket& s, std::string& out);

// Sends all of `data`, blocking. Returns false when the connection is gone.
bool send_all(Socket& s, std::string_view data);

// Blocking client connection to 127.0.0.1:`port`. Throws NetError.
Socket connect_loopback(std::uint16_t port);

// Blocking read of one line ('\n' excluded) into `line`, keeping extra bytes in `buffer`.
// Returns false on timeout (`timeout_ms` < 0 waits forever); throws NetError when the
// connection closes.
bool read_line(Socket& s, std::string& buffer, std::string& line, int timeout_ms);

}  // namespace anygba::control::net
