#include "control/server.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace anygba::control {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxLine = 8u << 20;

bool same_token(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned>(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace

std::string random_token() {
    std::random_device rd;
    std::array<std::uint8_t, 24> bytes{};
    for (auto& b : bytes) b = static_cast<std::uint8_t>(rd());
    return hex_encode(bytes.data(), bytes.size());
}

int current_pid() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

void write_file_atomic(const fs::path& path, std::string_view bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
#if !defined(_WIN32)
    ::chmod(tmp.c_str(), 0600);
#endif
    fs::rename(tmp, path, ec);
    if (ec) throw std::runtime_error("cannot replace " + path.string() + ": " + ec.message());
}

void Server::start(std::string_view host, std::uint16_t port, const fs::path& token_file, Json info) {
    stop();
    auto [socket, bound] = net::listen_loopback(host, port);
    token_ = random_token();
    Json file = Json::object();
    file["protocol"] = kProtocolVersion;
    file["port"] = bound;
    file["token"] = token_;
    file["pid"] = current_pid();
    if (info.is_object()) {
        for (auto& [k, v] : info.as_object()) file[k] = std::move(v);
    }
    write_file_atomic(token_file, file.dump() + "\n");
    listener_ = std::move(socket);
    port_ = bound;
    token_file_ = token_file;
}

void Server::stop() {
    for (auto& c : conns_) {
        if (c->authed) events_.push_back("disconnected " + c->client);
    }
    conns_.clear();
    if (listener_.valid()) {
        listener_.close();
        std::error_code ec;
        fs::remove(token_file_, ec);
    }
    port_ = 0;
    token_.clear();
}

Server::Conn* Server::find(std::uint64_t conn) {
    for (auto& c : conns_) {
        if (c->id == conn) return c.get();
    }
    return nullptr;
}

bool Server::connected(std::uint64_t conn) const {
    return std::any_of(conns_.begin(), conns_.end(), [conn](const auto& c) { return c->id == conn && !c->closing; });
}

std::size_t Server::clients() const {
    return static_cast<std::size_t>(std::count_if(conns_.begin(), conns_.end(), [](const auto& c) { return c->authed && !c->closing; }));
}

std::vector<std::string> Server::take_events() { return std::exchange(events_, {}); }

void Server::send(Conn& c, const Json& message) {
    if (c.closing) return;
    if (!net::send_all(c.socket, message.dump() + "\n")) c.closing = true;
}

void Server::reply(const Request& request, Json result) {
    Conn* c = find(request.conn);
    if (!c) return;
    Json m = Json::object();
    m["jsonrpc"] = "2.0";
    m["id"] = request.id;
    m["result"] = std::move(result);
    send(*c, m);
    c->waiting = false;
}

void Server::reply_error(const Request& request, int code, const std::string& message) {
    Conn* c = find(request.conn);
    if (!c) return;
    Json m = Json::object();
    m["jsonrpc"] = "2.0";
    m["id"] = request.id;
    Json e = Json::object();
    e["code"] = code;
    e["message"] = message;
    m["error"] = std::move(e);
    send(*c, m);
    c->waiting = false;
}

void Server::handle_line(Conn& c, std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (line.empty()) return;
    Request r;
    r.conn = c.id;
    r.client = c.client;
    const auto fail = [&](int code, const std::string& msg) {
        Json m = Json::object();
        m["jsonrpc"] = "2.0";
        m["id"] = r.id;
        Json e = Json::object();
        e["code"] = code;
        e["message"] = msg;
        m["error"] = std::move(e);
        send(c, m);
    };
    Json msg;
    try {
        msg = Json::parse(line);
    } catch (const JsonError& e) {
        fail(kParseError, e.what());
        return;
    }
    if (const Json* id = msg.find("id")) r.id = *id;
    const Json* method = msg.find("method");
    if (!msg.is_object() || !method || !method->is_string()) {
        fail(kInvalidRequest, "a request needs a string 'method'");
        return;
    }
    r.method = method->as_string();
    if (const Json* params = msg.find("params"); params && !params->is_null()) {
        if (!params->is_object()) {
            fail(kInvalidParams, "'params' must be an object");
            return;
        }
        r.params = *params;
    } else {
        r.params = Json::object();
    }

    if (!c.authed) {
        const Json* token = r.params.find("token");
        if (r.method != "hello") {
            fail(kUnauthorized, "send 'hello' with the token from the token file first");
            c.closing = true;
            return;
        }
        if (!token || !token->is_string() || !same_token(token->as_string(), token_)) {
            fail(kUnauthorized, "wrong token");
            c.closing = true;
            return;
        }
        c.authed = true;
        if (const Json* client = r.params.find("client"); client && client->is_string()) {
            c.client = client->as_string().substr(0, 64);
        }
        if (c.client.empty()) c.client = "agent";
        r.client = c.client;
        events_.push_back("connected " + c.client);
    }
    c.queued.push_back(std::move(r));
}

void Server::read(Conn& c) {
    if (!net::read_available(c.socket, c.in)) {
        c.closing = true;
    }
    std::size_t start = 0;
    for (;;) {
        const auto nl = c.in.find('\n', start);
        if (nl == std::string::npos) break;
        handle_line(c, std::string_view(c.in).substr(start, nl - start));
        start = nl + 1;
        if (c.closing) break;
    }
    c.in.erase(0, start);
    if (c.in.size() > kMaxLine) {
        Request r;
        r.conn = c.id;
        Json m = Json::object();
        m["jsonrpc"] = "2.0";
        m["id"] = nullptr;
        m["error"] = Json(Json::Object{{"code", kInvalidRequest}, {"message", "request line too long"}});
        send(c, m);
        c.closing = true;
    }
}

std::vector<Request> Server::poll(int timeout_ms) {
    std::vector<Request> out;
    if (!listener_.valid()) return out;
    // Requests already read but held back go out first, without waiting.
    const bool ready = std::any_of(conns_.begin(), conns_.end(), [](const auto& c) { return !c->waiting && !c->queued.empty(); });
    std::vector<const net::Socket*> sockets = {&listener_};
    for (const auto& c : conns_) sockets.push_back(&c->socket);
    for (const std::size_t i : net::wait_readable(sockets, ready ? 0 : timeout_ms)) {
        if (i == 0) {
            while (auto s = net::accept(listener_)) {
                auto c = std::make_unique<Conn>();
                c->id = next_id_++;
                c->socket = std::move(*s);
                conns_.push_back(std::move(c));
            }
        } else {
            read(*conns_[i - 1]);
        }
    }
    for (auto it = conns_.begin(); it != conns_.end();) {
        Conn& c = **it;
        if (c.closing) {
            if (c.authed) events_.push_back("disconnected " + c.client);
            it = conns_.erase(it);
            continue;
        }
        if (!c.waiting && !c.queued.empty()) {
            out.push_back(std::move(c.queued.front()));
            c.queued.pop_front();
            c.waiting = true;
        }
        ++it;
    }
    return out;
}

}  // namespace anygba::control
