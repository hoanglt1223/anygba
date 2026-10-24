// anygba-mcp: a Model Context Protocol server (stdio) that lets AI agents observe, play, control
// and debug running AnyGBA games and read their play journals (docs/user/AI-CONTROL.md). It
// speaks newline-delimited JSON-RPC to the agent on stdin/stdout and the game's control protocol
// (docs/dev/control-protocol.md) over 127.0.0.1.
#include "control/json.h"
#include "control/net.h"
#include "journal/journal.h"
#include "process.h"
#include "tools.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace net = anygba::control::net;
using anygba::control::Json;
using anygba::control::JsonError;

constexpr const char* kVersion = ANYGBA_VERSION;
constexpr const char* kProtocols[] = {"2025-06-18", "2025-03-26", "2024-11-05"};
constexpr int kCallTimeoutMs = 15 * 60 * 1000;  // step_frames may run 36000 frames paced

class ToolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::optional<fs::path> pref_dir() {
    char* path = SDL_GetPrefPath("anyport", "anygba");
    if (!path) return std::nullopt;
    fs::path p = fs::path(std::u8string(reinterpret_cast<const char8_t*>(path)));
    SDL_free(path);
    return p;
}

struct Options {
    fs::path control_dir;
    fs::path journal_dir;
    bool journal_dir_given = false;
};

// One attached game.
class Game {
public:
    Json info;
    std::string game;
    int pid = 0;

    void connect(std::uint16_t port, const std::string& token) {
        socket_ = net::connect_loopback(port);
        buffer_.clear();
        Json params = Json::object();
        params["token"] = token;
        params["client"] = "anygba-mcp";
        info = call("hello", std::move(params), 10000);
    }

    bool connected() const { return socket_.valid(); }
    void close() { socket_.close(); }

    // Throws ToolError with the game's message for a JSON-RPC error.
    Json call(const std::string& method, Json params, int timeout_ms = kCallTimeoutMs) {
        if (!connected()) throw ToolError("not attached to a game; use list_games and attach, or launch");
        const std::int64_t id = next_id_++;
        Json req = Json::object();
        req["jsonrpc"] = "2.0";
        req["id"] = id;
        req["method"] = method;
        req["params"] = params.is_object() ? std::move(params) : Json::object();
        if (!net::send_all(socket_, req.dump() + "\n")) {
            close();
            throw ToolError("the game closed the connection");
        }
        for (;;) {
            std::string line;
            try {
                if (!net::read_line(socket_, buffer_, line, timeout_ms)) {
                    throw ToolError("the game did not answer " + method + " in time");
                }
            } catch (const net::NetError& e) {
                close();
                throw ToolError(e.what());
            }
            Json msg = Json::parse(line);
            const Json* got = msg.find("id");
            if (!got || !got->is_int() || got->as_int() != id) continue;
            if (const Json* err = msg.find("error")) {
                const Json* m = err->find("message");
                throw ToolError(m && m->is_string() ? m->as_string() : err->dump());
            }
            const Json* result = msg.find("result");
            return result ? *result : Json();
        }
    }

private:
    net::Socket socket_;
    std::string buffer_;
    std::int64_t next_id_ = 1;
};

struct TokenFile {
    fs::path path;
    Json data;
    int pid = 0;
};

std::vector<TokenFile> scan_games(const fs::path& dir, bool remove_stale) {
    std::vector<TokenFile> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".json") continue;
        std::ifstream in(e.path(), std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        in.close();
        try {
            TokenFile t{e.path(), Json::parse(text), 0};
            const Json* pid = t.data.find("pid");
            t.pid = pid && pid->is_int() ? static_cast<int>(pid->as_int()) : 0;
            if (!anygba::mcp::process_alive(t.pid)) {
                if (remove_stale) fs::remove(e.path(), ec);  // left behind by a game that crashed
                continue;
            }
            out.push_back(std::move(t));
        } catch (const JsonError&) {
            // being written right now, or not ours
        }
    }
    return out;
}

std::string str_of(const Json& j, std::string_view key) {
    const Json* v = j.find(key);
    return v && v->is_string() ? v->as_string() : std::string();
}

class Server {
public:
    explicit Server(Options o) : opts_(std::move(o)) {}

    void run() {
        std::string line;
        while (std::getline(std::cin, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (line.empty()) continue;
            Json msg;
            try {
                msg = Json::parse(line);
            } catch (const JsonError& e) {
                send_error(Json(), -32700, e.what());
                continue;
            }
            if (msg.is_array()) {
                send_error(Json(), -32600, "batches are not supported");
                continue;
            }
            dispatch(msg);
        }
        game_.close();
    }

private:
    void send(const Json& msg) {
        const std::string text = msg.dump() + "\n";
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
    }

    void send_result(const Json& id, Json result) {
        Json m = Json::object();
        m["jsonrpc"] = "2.0";
        m["id"] = id;
        m["result"] = std::move(result);
        send(m);
    }

    void send_error(const Json& id, int code, const std::string& message) {
        Json m = Json::object();
        m["jsonrpc"] = "2.0";
        m["id"] = id;
        m["error"] = Json(Json::Object{{"code", code}, {"message", message}});
        send(m);
    }

    void dispatch(const Json& msg) {
        const Json* id = msg.find("id");
        const std::string method = str_of(msg, "method");
        const Json* params = msg.find("params");
        if (method.empty()) return;  // a response to nothing we sent
        if (!id) return;             // notifications (initialized, cancelled) need no answer
        const Json p = params && params->is_object() ? *params : Json::object();
        if (method == "initialize") {
            std::string version = kProtocols[0];
            const std::string asked = str_of(p, "protocolVersion");
            for (const char* v : kProtocols) {
                if (asked == v) version = v;
            }
            Json r = Json::object();
            r["protocolVersion"] = version;
            r["capabilities"] = Json(Json::Object{{"tools", Json::object()}});
            r["serverInfo"] = Json(Json::Object{{"name", "anygba-mcp"}, {"version", kVersion}});
            r["instructions"] =
                "AnyGBA games: list_games/attach or launch first. Then screenshot and get_state to look, press/input_sequence/"
                "step_frames to play (input is frame-accurate; pause stops at a frame), save_state before risky moves, "
                "read_named for game variables. journal_search and journal_summary show what earlier sessions learned; "
                "journal_note records goals and lessons. The user can take over at any time.";
            send_result(*id, std::move(r));
        } else if (method == "ping") {
            send_result(*id, Json::object());
        } else if (method == "tools/list") {
            Json list = Json::array();
            for (const auto& t : anygba::mcp::tools()) {
                Json item = Json::object();
                item["name"] = t.name;
                item["description"] = t.description;
                item["inputSchema"] = t.input_schema;
                item["annotations"] = Json(Json::Object{{"readOnlyHint", !t.mutating}, {"openWorldHint", false}});
                list.push_back(std::move(item));
            }
            send_result(*id, Json(Json::Object{{"tools", std::move(list)}}));
        } else if (method == "tools/call") {
            const std::string name = str_of(p, "name");
            const anygba::mcp::Tool* tool = anygba::mcp::find_tool(name);
            if (!tool) {
                send_error(*id, -32602, "unknown tool '" + name + "'");
                return;
            }
            const Json* args_v = p.find("arguments");
            const Json args = args_v && args_v->is_object() ? *args_v : Json::object();
            Json content = Json::array();
            bool error = false;
            try {
                content = call_tool(*tool, args);
            } catch (const std::exception& e) {
                content = Json::array();
                content.push_back(text(e.what()));
                error = true;
            }
            Json r = Json::object();
            r["content"] = std::move(content);
            r["isError"] = error;
            send_result(*id, std::move(r));
        } else {
            send_error(*id, -32601, "method not found: " + method);
        }
    }

    static Json text(const std::string& s) { return Json(Json::Object{{"type", "text"}, {"text", s}}); }
    static Json one(const Json& value) {
        Json c = Json::array();
        c.push_back(text(value.dump()));
        return c;
    }

    Json call_tool(const anygba::mcp::Tool& tool, const Json& args) {
        if (tool.name == "list_games") return one(list_games());
        if (tool.name == "attach") return one(attach(args));
        if (tool.name == "launch") return one(launch(args));
        if (tool.name == "detach") {
            game_.close();
            return one(Json(Json::Object{{"detached", true}}));
        }
        if (!game_.connected() && (tool.name == "journal_search" || tool.name == "journal_summary")) {
            return one(offline_journal(tool.name, args));
        }
        if (tool.name == "screenshot") {
            Json r = game_.call("screenshot", args);
            Json c = Json::array();
            c.push_back(Json(Json::Object{{"type", "image"}, {"data", str_of(r, "png_base64")}, {"mimeType", "image/png"}}));
            if (r.is_object()) std::erase_if(r.as_object(), [](const auto& kv) { return kv.first == "png_base64"; });
            c.push_back(text(r.dump()));
            return c;
        }
        Json r = game_.call(tool.name == "get_info" ? "get_info" : tool.name, args);
        if (tool.name == "quit" || (tool.name == "set_control" && args.find("enabled") && args.find("enabled")->is_bool() &&
                                    !args.find("enabled")->as_bool())) {
            game_.close();
        }
        return one(r);
    }

    Json list_games() {
        Json list = Json::array();
        for (const TokenFile& t : scan_games(opts_.control_dir, true)) {
            Json g = Json::object();
            g["game"] = str_of(t.data, "game");
            g["title"] = str_of(t.data, "title");
            g["pid"] = t.pid;
            g["port"] = t.data.find("port") ? *t.data.find("port") : Json();
            g["exe"] = str_of(t.data, "exe");
            g["readonly"] = t.data.find("readonly") ? *t.data.find("readonly") : Json(false);
            g["attached"] = game_.connected() && game_.pid == t.pid;
            list.push_back(std::move(g));
        }
        return Json(Json::Object{{"games", std::move(list)}, {"control_dir", opts_.control_dir.string()}});
    }

    Json attach_to(const TokenFile& t) {
        game_.close();
        const Json* port = t.data.find("port");
        if (!port || !port->is_int() || port->as_int() <= 0 || port->as_int() > 65535) {
            throw ToolError("bad token file " + t.path.string());
        }
        game_.connect(static_cast<std::uint16_t>(port->as_int()), str_of(t.data, "token"));
        game_.pid = t.pid;
        game_.game = str_of(t.data, "game");
        Json out = game_.info;
        if (out.is_object()) std::erase_if(out.as_object(), [](const auto& kv) { return kv.first == "methods"; });
        out["attached"] = true;
        return out;
    }

    Json attach(const Json& args) {
        std::vector<TokenFile> games = scan_games(opts_.control_dir, true);
        const std::string want = str_of(args, "game");
        const Json* pid = args.find("pid");
        std::vector<TokenFile> match;
        for (auto& t : games) {
            if (pid && pid->is_int() && t.pid != pid->as_int()) continue;
            if (!want.empty() && str_of(t.data, "game") != want && str_of(t.data, "title").find(want) == std::string::npos) continue;
            match.push_back(t);
        }
        if (match.empty()) {
            throw ToolError("no running game with AI control on" + std::string(want.empty() ? "" : " matching '" + want + "'") +
                            " in " + opts_.control_dir.string() +
                            " (turn it on in the game: menu Settings > AI control, or F10; or use launch)");
        }
        if (match.size() > 1) throw ToolError("several games match; pass game or pid (list_games)");
        return attach_to(match.front());
    }

    Json launch(const Json& args) {
        const std::string exe = str_of(args, "exe");
        if (exe.empty() || !fs::exists(exe)) throw ToolError("exe must name an existing game executable");
        std::vector<std::string> argv = {exe};
        if (const Json* extra = args.find("args")) {
            if (!extra->is_array()) throw ToolError("args must be a list of strings");
            for (const Json& a : extra->as_array()) {
                if (!a.is_string()) throw ToolError("args must be a list of strings");
                argv.push_back(a.as_string());
            }
        }
        const auto flag = [&](const char* key) { return args.find(key) && args.find(key)->is_bool() && args.find(key)->as_bool(); };
        if (flag("headless")) argv.push_back("--headless");
        if (flag("journal")) argv.push_back("--journal");
        if (flag("readonly")) argv.push_back("--control-readonly");
        argv.insert(argv.end(), {"--control", "--control-dir", opts_.control_dir.string()});
        if (opts_.journal_dir_given) argv.insert(argv.end(), {"--journal-dir", opts_.journal_dir.string()});
        std::error_code ec;
        fs::create_directories(opts_.control_dir, ec);
        const fs::path log = opts_.control_dir / (fs::path(exe).stem().string() + ".log");
        const int pid = anygba::mcp::spawn(argv, log);
        const Json* wait_v = args.find("wait_seconds");
        const auto wait = std::chrono::seconds(wait_v && wait_v->is_int() ? std::clamp<std::int64_t>(wait_v->as_int(), 1, 600) : 30);
        const auto until = std::chrono::steady_clock::now() + wait;
        while (std::chrono::steady_clock::now() < until) {
            for (const TokenFile& t : scan_games(opts_.control_dir, false)) {
                if (t.pid == pid) {
                    Json out = attach_to(t);
                    out["log"] = log.string();
                    return out;
                }
            }
            if (!anygba::mcp::process_alive(pid)) {
                throw ToolError("the game exited at start; see " + log.string());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        throw ToolError("the game did not turn AI control on within " + std::to_string(wait.count()) + " s; see " + log.string());
    }

    Json offline_journal(const std::string& tool, const Json& args) {
        const std::string game = str_of(args, "game") == "all" ? std::string() : str_of(args, "game");
        if (tool == "journal_search") {
            anygba::journal::Query q;
            q.text = str_of(args, "text");
            q.kind = str_of(args, "kind");
            q.who = str_of(args, "who");
            q.method = str_of(args, "method");
            q.session = str_of(args, "session");
            q.game = game;
            if (const Json* l = args.find("limit"); l && l->is_int()) q.limit = static_cast<std::size_t>(std::max<std::int64_t>(1, l->as_int()));
            return Json(Json::Object{{"records", anygba::journal::search(opts_.journal_dir, q)}});
        }
        const auto sessions = anygba::journal::list_sessions(opts_.journal_dir, game);
        const std::string session = str_of(args, "session");
        for (const auto& s : sessions) {
            if (session.empty() || s.id == session) return anygba::journal::summarize(s.dir);
        }
        throw ToolError("no journal session found in " + opts_.journal_dir.string());
    }

    Options opts_;
    Game game_;
};

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    Options opts;
    std::optional<fs::path> control, journal;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--control-dir" && i + 1 < argc) {
            control = fs::path(argv[++i]);
        } else if (a == "--journal-dir" && i + 1 < argc) {
            journal = fs::path(argv[++i]);
        } else if (a == "--version") {
            std::printf("anygba-mcp %s\n", kVersion);
            return 0;
        } else {
            std::fprintf(stderr,
                         "usage: anygba-mcp [--control-dir DIR] [--journal-dir DIR]\n"
                         "An MCP server on stdin/stdout for AI agents to play and debug AnyGBA games.\n"
                         "Defaults: <per-user data folder>/control and /journal, the folders the games use.\n");
            return 2;
        }
    }
    const auto pref = pref_dir();
    if ((!control || !journal) && !pref) {
        std::fprintf(stderr, "anygba-mcp: no per-user data folder; pass --control-dir and --journal-dir\n");
        return 1;
    }
    opts.control_dir = control ? *control : *pref / "control";
    opts.journal_dir = journal ? *journal : *pref / "journal";
    opts.journal_dir_given = journal.has_value();
    try {
        Server(std::move(opts)).run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "anygba-mcp: %s\n", e.what());
        return 1;
    }
    return 0;
}
