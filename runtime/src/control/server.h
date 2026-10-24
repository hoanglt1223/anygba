#pragma once

#include "control/json.h"
#include "control/net.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The control server's transport (docs/dev/control-protocol.md): newline-delimited JSON-RPC
// 2.0 over TCP on 127.0.0.1, a random token in a per-user file, and a `hello` handshake. It
// is polled from the game's main thread, so no locking is needed anywhere. Each connection
// gets one request at a time: the next one is handed out after the reply to the previous.
namespace anygba::control {

inline constexpr int kProtocolVersion = 1;

// JSON-RPC error codes.
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kUnauthorized = -32001;
inline constexpr int kReadOnly = -32002;
inline constexpr int kFailed = -32003;

struct Request {
    std::uint64_t conn = 0;
    Json id;
    std::string method;
    Json params;  // always an object
    std::string client;
};

class Server {
public:
    Server() = default;
    ~Server() { stop(); }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Listens and writes the token file {protocol, port, token, pid, ...info}. Throws
    // std::runtime_error (non-loopback host, port in use, unwritable token file).
    void start(std::string_view host, std::uint16_t port, const std::filesystem::path& token_file, Json info);
    // Closes the listener and every connection and deletes the token file.
    void stop();
    bool running() const { return listener_.valid(); }
    std::uint16_t port() const { return port_; }
    const std::string& token() const { return token_; }

    // Accepts, reads, answers `hello`; returns the next request of every authenticated
    // connection that is not waiting for a reply. Waits up to `timeout_ms` for input.
    std::vector<Request> poll(int timeout_ms);
    void reply(const Request& request, Json result);
    void reply_error(const Request& request, int code, const std::string& message);
    bool connected(std::uint64_t conn) const;
    std::size_t clients() const;
    // "connected <client>" / "disconnected <client>" since the last call.
    std::vector<std::string> take_events();

private:
    struct Conn {
        std::uint64_t id = 0;
        net::Socket socket;
        std::string in;
        bool authed = false;
        bool waiting = false;  // a request was handed out and not answered yet
        bool closing = false;
        std::deque<Request> queued;
        std::string client;
    };
    void read(Conn& c);
    void handle_line(Conn& c, std::string_view line);
    void send(Conn& c, const Json& message);
    Conn* find(std::uint64_t conn);

    net::Socket listener_;
    std::uint16_t port_ = 0;
    std::string token_;
    std::filesystem::path token_file_;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::uint64_t next_id_ = 1;
    std::vector<std::string> events_;
};

std::string random_token();
int current_pid();
// Writes `bytes` to `path` through a temporary file; throws std::runtime_error.
void write_file_atomic(const std::filesystem::path& path, std::string_view bytes);

}  // namespace anygba::control
