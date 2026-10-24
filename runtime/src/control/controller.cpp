#include "control/controller.h"

#include "anygba/sha256.h"
#include "control/memory.h"
#include "control/png.h"
#include "decode/arm.h"
#include "decode/disasm.h"
#include "decode/thumb.h"
#include "input_map.h"
#include "player/speed.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace anygba::control {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kLogLines = 1000;
constexpr std::uint32_t kMaxFrames = 36000;  // ten minutes of game time per command

class ParamError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string hex32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

const Json* param(const Json& p, std::string_view key) {
    const Json* v = p.find(key);
    return v && !v->is_null() ? v : nullptr;
}

// Integers as JSON numbers or as strings ("0x0202..." or decimal), since agents use both.
std::int64_t integer(const Json& v, std::string_view key) {
    if (v.is_number()) {
        try {
            return v.as_int();
        } catch (const JsonError&) {
            throw ParamError(std::string(key) + " must be an integer");
        }
    }
    if (v.is_string()) {
        const std::string& s = v.as_string();
        char* end = nullptr;
        const bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
        const long long n = std::strtoll(s.c_str(), &end, hex ? 16 : 10);
        if (!s.empty() && *end == '\0') return n;
    }
    throw ParamError(std::string(key) + " must be an integer (a number or a string such as \"0x02024000\")");
}

std::int64_t int_param(const Json& p, std::string_view key, std::optional<std::int64_t> def, std::int64_t min, std::int64_t max) {
    const Json* v = param(p, key);
    if (!v) {
        if (!def) throw ParamError("missing '" + std::string(key) + "'");
        return *def;
    }
    const std::int64_t n = integer(*v, key);
    if (n < min || n > max) {
        throw ParamError(std::string(key) + " must be from " + std::to_string(min) + " to " + std::to_string(max));
    }
    return n;
}

std::uint32_t addr_param(const Json& p, std::string_view key = "address") {
    return static_cast<std::uint32_t>(int_param(p, key, std::nullopt, 0, 0xFFFFFFFFll));
}

std::string str_param(const Json& p, std::string_view key, std::optional<std::string> def = std::nullopt) {
    const Json* v = param(p, key);
    if (!v) {
        if (!def) throw ParamError("missing '" + std::string(key) + "'");
        return *def;
    }
    if (!v->is_string()) throw ParamError(std::string(key) + " must be a string");
    return v->as_string();
}

bool bool_param(const Json& p, std::string_view key, std::optional<bool> def) {
    const Json* v = param(p, key);
    if (!v) {
        if (!def) throw ParamError("missing '" + std::string(key) + "'");
        return *def;
    }
    if (!v->is_bool()) throw ParamError(std::string(key) + " must be true or false");
    return v->as_bool();
}

// Long strings (memory dumps, pictures) are shortened in the journal.
Json brief(const Json& v, int depth = 0) {
    if (v.is_string() && v.as_string().size() > 256) {
        return Json(v.as_string().substr(0, 64) + "...(" + std::to_string(v.as_string().size()) + " chars)");
    }
    if (depth < 4 && v.is_object()) {
        Json o = Json::object();
        for (const auto& [k, x] : v.as_object()) o[k] = brief(x, depth + 1);
        return o;
    }
    if (depth < 4 && v.is_array()) {
        Json a = Json::array();
        std::size_t n = 0;
        for (const Json& x : v.as_array()) {
            if (++n > 64) {
                a.push_back("...(" + std::to_string(v.size()) + " items)");
                break;
            }
            a.push_back(brief(x, depth + 1));
        }
        return a;
    }
    return v;
}

bool valid_state_name(const std::string& s) {
    return !s.empty() && s.size() <= 64 &&
           std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
}

std::string frame_hash(System& sys) {
    const auto& fb = sys.framebuffer();
    return sha256_hex({reinterpret_cast<const std::uint8_t*>(fb.data()), fb.size() * sizeof(fb[0])});
}

bool writable_region(std::uint32_t addr) {
    const std::uint32_t r = addr >> 24;
    return r == 0x02 || r == 0x03 || (r == 0x04 && addr < 0x04000400) || r == 0x05 || r == 0x06 || r == 0x07;
}

}  // namespace

std::uint16_t parse_buttons(const Json& buttons) {
    std::vector<std::string> names;
    if (buttons.is_string()) {
        std::string cur;
        for (const char c : buttons.as_string() + "+") {
            if (c == '+' || c == ',' || c == ' ') {
                if (!cur.empty()) names.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
    } else if (buttons.is_array()) {
        for (const Json& b : buttons.as_array()) {
            if (!b.is_string()) throw ParamError("buttons must be names such as \"A\" or \"Up\"");
            names.push_back(b.as_string());
        }
    } else if (!buttons.is_null()) {
        throw ParamError("buttons must be a list of names or a string such as \"A+Up\"");
    }
    std::uint16_t mask = 0;
    for (const std::string& n : names) {
        int found = -1;
        for (int i = 0; i < input::kGbaButtons; ++i) {
            const std::string_view want = input::kGbaButtonNames[static_cast<std::size_t>(i)];
            if (n.size() == want.size() && std::equal(n.begin(), n.end(), want.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                })) {
                found = i;
            }
        }
        if (found < 0) throw ParamError("unknown button '" + n + "' (A, B, Select, Start, Right, Left, Up, Down, R, L)");
        mask = static_cast<std::uint16_t>(mask | (1u << found));
    }
    return mask;
}

std::string button_names(std::uint16_t mask) {
    std::string out;
    for (int i = 0; i < input::kGbaButtons; ++i) {
        if ((mask >> i) & 1u) {
            if (!out.empty()) out += '+';
            out += input::kGbaButtonNames[static_cast<std::size_t>(i)];
        }
    }
    return out;
}

Controller::Controller(System& sys, cheats::CheatList& cheats, Hooks hooks, Settings settings, Symbols symbols,
                       std::unique_ptr<journal::Journal> journal)
    : sys_(sys),
      cheats_(cheats),
      hooks_(std::move(hooks)),
      settings_(std::move(settings)),
      symbols_(std::move(symbols)),
      journal_(std::move(journal)) {
    sys_.on_breakpoint = [this](System&, std::uint32_t addr) {
        for (auto& [id, bp] : breakpoints_) {
            if (bp.addr != addr) continue;
            ++bp.hits;
            log("breakpoint " + std::to_string(id) + " at " + hex32(addr) + (bp.stop ? " (stopped)" : ""));
            if (bp.stop) {
                Json reason = Json::object();
                reason["reason"] = "breakpoint";
                reason["id"] = id;
                reason["address"] = hex32(addr);
                stop_here(std::move(reason));
            }
            return;
        }
    };
    sys_.on_watch = [this](System&, const System::Access& a) { on_access(a); };
}

Controller::~Controller() {
    sys_.on_breakpoint = nullptr;
    sys_.on_watch = nullptr;
    if (journal_on()) journal_->stop(sys_.frames());
    server_.stop();
}

// ---------------------------------------------------------------- toggles

std::optional<std::string> Controller::set_control(bool on, std::string_view who) {
    if (on == control_on()) return std::nullopt;
    if (on) {
        try {
            Json info = settings_.info;
            info["readonly"] = settings_.readonly;
            server_.start(settings_.host, settings_.port, settings_.token_file, std::move(info));
        } catch (const std::exception& e) {
            const std::string msg = std::string("AI control could not start: ") + e.what();
            log(msg);
            if (hooks_.osd) hooks_.osd(msg);
            return msg;
        }
        log("AI control on: 127.0.0.1:" + std::to_string(server_.port()) + ", token file " + settings_.token_file.string());
        if (hooks_.osd) hooks_.osd("AI control on (port " + std::to_string(server_.port()) + ")");
        user_event("control", Json(Json::Object{{"enabled", true}, {"port", server_.port()}}), who);
    } else {
        server_.stop();
        for (const std::string& e : server_.take_events()) log(e);
        drop_agent_state();
        log("AI control off");
        if (hooks_.osd) hooks_.osd("AI control off");
        user_event("control", Json(Json::Object{{"enabled", false}}), who);
    }
    return std::nullopt;
}

std::optional<std::string> Controller::set_journal(bool on, std::string_view who) {
    if (!journal_) return "no folder for the journal";
    if (on == journal_on()) return std::nullopt;
    if (on) {
        try {
            Json info = settings_.info;
            info["by"] = who;
            journal_->start(sys_.frames(), std::move(info));
        } catch (const std::exception& e) {
            const std::string msg = std::string("journal could not start: ") + e.what();
            log(msg);
            if (hooks_.osd) hooks_.osd(msg);
            return msg;
        }
        log("journal on: " + journal_->session_dir().string());
        if (hooks_.osd) hooks_.osd("Journal on");
        Json active = Json::array();
        for (const auto& c : cheats_.cheats()) {
            if (c.enabled) active.push_back(c.name);
        }
        if (active.size()) journal_->record(sys_.frames(), "cheats_active", "cheat", Json(Json::Object{{"cheats", std::move(active)}}));
        journal_->thumbnail(sys_.frames(), sys_.framebuffer(), "session_start");
    } else {
        journal_->record(sys_.frames(), "journal", who, Json(Json::Object{{"enabled", false}}));
        journal_->stop(sys_.frames());
        log("journal off");
        if (hooks_.osd) hooks_.osd("Journal off");
    }
    return std::nullopt;
}

void Controller::set_readonly(bool on, std::string_view who) {
    if (settings_.readonly == on) return;
    settings_.readonly = on;
    if (on) {
        hold_ = 0;
        timed_.clear();
    }
    log(std::string("read-only agent mode ") + (on ? "on" : "off"));
    if (hooks_.osd) hooks_.osd(std::string("Read-only agent ") + (on ? "on" : "off"));
    user_event("readonly", Json(Json::Object{{"enabled", on}}), who);
}

void Controller::drop_agent_state() {
    hold_ = 0;
    timed_.clear();
    pending_.clear();
    paused_ = false;
    run_until_ = 0;
    stop_reason_ = Json();
    breakpoints_.clear();
    watches_.clear();
    sync_breakpoints();
    sync_watches();
}

// ---------------------------------------------------------------- per frame

std::uint16_t Controller::agent_mask(std::uint64_t frame) const {
    std::uint16_t mask = hold_;
    for (const Timed& t : timed_) {
        if (frame >= t.from && frame < t.until) mask = static_cast<std::uint16_t>(mask | t.mask);
    }
    return mask;
}

std::uint16_t Controller::keys(std::uint16_t user_keys) {
    const std::uint64_t frame = sys_.frames();
    std::uint16_t agent = control_on() && !settings_.readonly ? agent_mask(frame) : 0;
    if (user_keys) {
        if (agent && frame >= takeover_until_) {
            log("user input: agent input paused for " + std::to_string(settings_.takeover_frames) + " frames");
            if (hooks_.osd) hooks_.osd("You have control");
            user_event("takeover", Json(Json::Object{{"buttons", button_names(user_keys)}}));
        }
        takeover_until_ = frame + static_cast<std::uint64_t>(settings_.takeover_frames);
    }
    if (frame < takeover_until_) agent = 0;
    last_agent_mask_ = agent;
    return static_cast<std::uint16_t>(user_keys | agent);
}

void Controller::service_frame() {
    const std::uint64_t frame = sys_.frames();
    if (journal_on()) {
        journal_->on_frame(frame, [this] { return symbols_.snapshot(peeker()); }, sys_.framebuffer());
    }
    if (!control_on()) return;
    poll_and_handle(0);
    complete_due();
    if (paused_ && sys_.frames() >= run_until_) wait_while_paused(Where::Frame);
}

void Controller::complete_due() {
    const std::uint64_t frame = sys_.frames();
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (!server_.connected(it->request.conn)) {
            it = pending_.erase(it);
        } else if (frame >= it->until) {
            Json result = std::move(it->result);
            result["frame"] = frame;
            if (takeover_until_ > it->from) result["user_override"] = true;
            const Request r = std::move(it->request);
            it = pending_.erase(it);
            finish(r, std::move(result));
        } else {
            ++it;
        }
    }
    std::erase_if(timed_, [frame](const Timed& t) { return t.until <= frame; });
}

void Controller::poll_and_handle(int timeout_ms) {
    for (const Request& r : server_.poll(timeout_ms)) {
        handle(r);
    }
    for (const std::string& e : server_.take_events()) {
        log(e);
        const bool connected = e.starts_with("connected");
        if (hooks_.osd) hooks_.osd(std::string(connected ? "AI connected: " : "AI disconnected: ") + e.substr(e.find(' ') + 1));
        user_event(connected ? "agent_connected" : "agent_disconnected", Json(Json::Object{{"client", e.substr(e.find(' ') + 1)}}), "agent");
    }
    if (control_on() && server_.clients() == 0 && (paused_ || hold_ || !timed_.empty() || !breakpoints_.empty() || !watches_.empty())) {
        // The last agent left: nothing it set up may keep holding the game.
        drop_agent_state();
        log("no agent connected: released input, resumed, cleared breakpoints and watches");
    }
}

void Controller::wait_while_paused(Where where) {
    const Where saved = where_;
    where_ = where;
    while (control_on() && paused_ && sys_.frames() >= run_until_ && !sys_.safe_point_pending()) {
        poll_and_handle(15);
        std::uint16_t user = 0;
        if (hooks_.idle && !hooks_.idle(user)) {
            paused_ = false;
            sys_.stop();
            break;
        }
        if (user && paused_) {
            paused_ = false;
            stop_reason_ = Json();
            takeover_until_ = sys_.frames() + static_cast<std::uint64_t>(settings_.takeover_frames);
            log("user input while the agent paused the game: resumed");
            if (hooks_.osd) hooks_.osd("You have control");
            user_event("takeover", Json(Json::Object{{"buttons", button_names(user)}, {"resumed", true}}));
        }
    }
    where_ = saved;
}

void Controller::stop_here(Json reason) {
    if (servicing_ || !control_on() || where_ == Where::SafePoint) return;
    // Commands waiting for frames cannot finish while stopped; answer them now.
    for (Pending& p : pending_) {
        Json result = std::move(p.result);
        result["frame"] = sys_.frames();
        result["interrupted"] = reason;
        finish(p.request, std::move(result));
    }
    pending_.clear();
    paused_ = true;
    run_until_ = 0;
    stop_reason_ = reason;
    if (hooks_.osd) hooks_.osd("AI: stopped (" + reason.find("reason")->as_string() + ")");
    if (journal_on()) {
        journal_->record(sys_.frames(), "stop", "agent", Json(Json::Object{{"reason", reason}}));
        journal_->thumbnail(sys_.frames(), sys_.framebuffer(), "stop");
    }
    wait_while_paused(Where::Stop);
}

void Controller::at_safe_point(const Request& r, std::function<Json(System&)> op) {
    if (where_ == Where::SafePoint) {
        finish(r, op(sys_));
        return;
    }
    if (sys_.safe_point_pending()) {
        fail(r, kFailed, "another save or load is in progress; try again");
        return;
    }
    sys_.request_safe_point([this, r, op = std::move(op)](System& s) {
        const Where saved = where_;
        where_ = Where::SafePoint;
        servicing_ = true;
        try {
            finish(r, op(s));
        } catch (const std::exception& e) {
            fail(r, kFailed, e.what());
        }
        servicing_ = false;
        where_ = saved;
        if (paused_ && control_on()) wait_while_paused(Where::SafePoint);
    });
}

// ---------------------------------------------------------------- requests

void Controller::handle(const Request& r) {
    const auto& table = methods();
    const auto it = table.find(r.method);
    if (it == table.end()) {
        fail(r, kMethodNotFound, "unknown method '" + r.method + "'");
        return;
    }
    if (settings_.readonly && (it->second.category == Category::Play || it->second.category == Category::Control)) {
        fail(r, kReadOnly, "read-only agent mode: only observe, debug and journal commands are allowed");
        return;
    }
    servicing_ = true;
    try {
        it->second.handler(*this, r);
    } catch (const ParamError& e) {
        fail(r, kInvalidParams, e.what());
    } catch (const std::exception& e) {
        fail(r, kFailed, e.what());
    }
    servicing_ = false;
}

void Controller::journal_command(const Request& r, const Json* result, const std::string* error) {
    if (!journal_on()) return;
    Json data = Json::object();
    data["method"] = r.method;
    data["client"] = r.client;
    Json params = brief(r.params);
    if (params.is_object()) std::erase_if(params.as_object(), [](const auto& kv) { return kv.first == "token"; });
    data["params"] = std::move(params);
    if (result) {
        if (r.method == "screenshot") {
            Json b = Json::object();
            for (const char* k : {"width", "height", "frame", "frame_hash"}) {
                if (const Json* v = result->find(k)) b[k] = *v;
            }
            data["result"] = std::move(b);
        } else {
            data["result"] = brief(*result);
        }
    }
    if (error) data["error"] = *error;
    journal_->record(sys_.frames(), "command", "agent", std::move(data));
}

void Controller::finish(const Request& r, Json result) {
    journal_command(r, &result, nullptr);
    server_.reply(r, std::move(result));
}

void Controller::fail(const Request& r, int code, const std::string& message) {
    journal_command(r, nullptr, &message);
    log("error: " + r.method + ": " + message);
    server_.reply_error(r, code, message);
}

void Controller::user_event(std::string_view kind, Json data, std::string_view who) {
    if (journal_on()) journal_->record(sys_.frames(), kind, who, std::move(data));
}

void Controller::log(std::string line) {
    char prefix[32];
    std::snprintf(prefix, sizeof prefix, "[%llu] ", static_cast<unsigned long long>(sys_.frames()));
    log_.push_back(prefix + line);
    if (log_.size() > kLogLines) log_.pop_front();
}

std::string Controller::indicator() const {
    if (!control_on() || server_.clients() == 0) return {};
    std::string s = "AI";
    if (paused_) s += " paused";
    if (last_agent_mask_) s += " " + button_names(last_agent_mask_);
    if (settings_.readonly) s += " (read-only)";
    return s;
}

Peek Controller::peeker() {
    return [this](std::uint32_t addr, std::span<std::uint8_t> out) { peek(sys_, addr, out); };
}

Json Controller::info() const {
    Json i = settings_.info;
    i["protocol"] = kProtocolVersion;
    i["pid"] = current_pid();
    i["readonly"] = settings_.readonly;
    i["journal"] = journal_ && journal_->active();
    if (journal_) i["journal_root"] = journal_->root().string();
    i["symbols"] = symbols_.all().size();
    Json m = Json::array();
    for (const auto& [name, method] : methods()) m.push_back(name);
    i["methods"] = std::move(m);
    return i;
}

Json Controller::state_json() {
    Json s = Json::object();
    s["frame"] = sys_.frames();
    s["fps"] = hooks_.fps ? hooks_.fps() : 0.0;
    s["speed"] = hooks_.speed ? hooks_.speed() : 100;
    s["paused"] = paused_;
    if (!stop_reason_.is_null()) s["stop_reason"] = stop_reason_;
    s["agent_buttons"] = button_names(agent_mask(sys_.frames() + 1));
    s["user_override_frames"] = takeover_until_ > sys_.frames() ? takeover_until_ - sys_.frames() : 0;
    s["pad"] = hooks_.pad_name ? hooks_.pad_name() : std::string();
    Json active = Json::array();
    for (const auto& c : cheats_.cheats()) {
        if (c.enabled) active.push_back(c.name);
    }
    s["active_cheats"] = std::move(active);
    s["readonly"] = settings_.readonly;
    s["journal"] = journal_on();
    if (journal_on()) s["journal_session"] = journal_->session_id();
    s["clients"] = server_.clients();
    s["frame_hash"] = frame_hash(sys_);
    return s;
}

const std::map<std::string, Controller::Method>& Controller::methods() {
    static const std::map<std::string, Method> table = {
        {"hello", {Category::Session, &Controller::h_hello}},
        {"get_info", {Category::Session, &Controller::h_hello}},
        {"set_control", {Category::Session, &Controller::h_set_control}},
        {"set_journal", {Category::Session, &Controller::h_set_journal}},
        {"set_readonly", {Category::Session, &Controller::h_set_readonly}},
        {"get_state", {Category::Observe, &Controller::h_get_state}},
        {"screenshot", {Category::Observe, &Controller::h_screenshot}},
        {"read_memory", {Category::Observe, &Controller::h_read_memory}},
        {"read_named", {Category::Observe, &Controller::h_read_named}},
        {"list_symbols", {Category::Observe, &Controller::h_list_symbols}},
        {"list_states", {Category::Observe, &Controller::h_list_states}},
        {"list_cheats", {Category::Observe, &Controller::h_list_cheats}},
        {"press", {Category::Play, &Controller::h_press}},
        {"hold", {Category::Play, &Controller::h_hold}},
        {"release", {Category::Play, &Controller::h_release}},
        {"input_sequence", {Category::Play, &Controller::h_input_sequence}},
        {"step_frames", {Category::Play, &Controller::h_step_frames}},
        {"pause", {Category::Play, &Controller::h_pause}},
        {"resume", {Category::Play, &Controller::h_resume}},
        {"set_speed", {Category::Play, &Controller::h_set_speed}},
        {"save_state", {Category::Control, &Controller::h_save_state}},
        {"load_state", {Category::Control, &Controller::h_load_state}},
        {"set_cheat", {Category::Control, &Controller::h_set_cheat}},
        {"write_memory", {Category::Control, &Controller::h_write_memory}},
        {"journal_clear", {Category::Control, &Controller::h_journal_clear}},
        {"quit", {Category::Control, &Controller::h_quit}},
        {"get_registers", {Category::Debug, &Controller::h_get_registers}},
        {"disassemble", {Category::Debug, &Controller::h_disassemble}},
        {"watch_memory", {Category::Debug, &Controller::h_watch_memory}},
        {"break_at_function", {Category::Debug, &Controller::h_break_at_function}},
        {"clear_debug", {Category::Debug, &Controller::h_clear_debug}},
        {"list_debug", {Category::Debug, &Controller::h_list_debug}},
        {"get_call_stack", {Category::Debug, &Controller::h_get_call_stack}},
        {"get_miss_log", {Category::Debug, &Controller::h_get_miss_log}},
        {"tail_log", {Category::Debug, &Controller::h_tail_log}},
        {"journal_note", {Category::Journal, &Controller::h_journal_note}},
        {"journal_summary", {Category::Journal, &Controller::h_journal_summary}},
        {"journal_search", {Category::Journal, &Controller::h_journal_search}},
        {"journal_export", {Category::Journal, &Controller::h_journal_export}},
    };
    return table;
}

// ---------------------------------------------------------------- session

void Controller::h_hello(const Request& r) {
    Json i = info();
    i["frame"] = sys_.frames();
    finish(r, std::move(i));
}

void Controller::h_set_control(const Request& r) {
    const bool on = bool_param(r.params, "enabled", std::nullopt);
    finish(r, Json(Json::Object{{"enabled", on}}));
    if (!on) set_control(false, "agent");
}

void Controller::h_set_journal(const Request& r) {
    if (!journal_) throw std::runtime_error("this game has no folder for the journal");
    journal::Settings& s = journal_->settings();
    s.snapshot_seconds = static_cast<int>(int_param(r.params, "snapshot_seconds", s.snapshot_seconds, 0, 3600));
    s.thumbnail_seconds = static_cast<int>(int_param(r.params, "thumbnail_seconds", s.thumbnail_seconds, 0, 3600));
    s.max_mb = static_cast<std::uint64_t>(int_param(r.params, "max_mb", static_cast<std::int64_t>(s.max_mb), 1, 1 << 20));
    s.max_days = static_cast<int>(int_param(r.params, "max_days", s.max_days, 0, 3650));
    const bool on = bool_param(r.params, "enabled", journal_on());
    if (on == journal_on()) {
        if (on) journal_->record(sys_.frames(), "journal", "agent", Json(Json::Object{{"settings_changed", true}}));
    } else if (const auto error = set_journal(on, "agent")) {
        throw std::runtime_error(*error);
    }
    Json out = Json::object();
    out["enabled"] = journal_on();
    if (journal_on()) {
        out["session"] = journal_->session_id();
        out["dir"] = journal_->session_dir().string();
    }
    finish(r, std::move(out));
}

void Controller::h_set_readonly(const Request& r) {
    if (!bool_param(r.params, "enabled", std::nullopt)) {
        throw std::runtime_error("only the user can turn read-only agent mode off (menu: Settings > Read-only agent)");
    }
    set_readonly(true, "agent");
    finish(r, Json(Json::Object{{"readonly", true}}));
}

// ---------------------------------------------------------------- observe

void Controller::h_get_state(const Request& r) { finish(r, state_json()); }

void Controller::h_screenshot(const Request& r) {
    const Json* scale_v = param(r.params, "scale");
    const double scale = scale_v ? scale_v->as_double() : 1.0;
    if (!(scale >= 0.25 && scale <= 4)) throw ParamError("scale must be from 0.25 to 4");
    const auto png = encode_png(sys_.framebuffer(), kScreenWidth, kScreenHeight, scale);
    Json out = Json::object();
    out["frame"] = sys_.frames();
    out["width"] = static_cast<int>(kScreenWidth * scale + 0.5);
    out["height"] = static_cast<int>(kScreenHeight * scale + 0.5);
    out["frame_hash"] = frame_hash(sys_);
    out["png_base64"] = base64_encode(png.data(), png.size());
    finish(r, std::move(out));
}

void Controller::h_read_memory(const Request& r) {
    const std::uint32_t addr = addr_param(r.params);
    const auto length = static_cast<std::size_t>(int_param(r.params, "length", 16, 1, 65536));
    std::vector<std::uint8_t> bytes(length);
    peek(sys_, addr, bytes);
    Json out = Json::object();
    out["address"] = hex32(addr);
    out["region"] = region_name(addr);
    out["length"] = length;
    out["hex"] = hex_encode(bytes.data(), bytes.size());
    if (length == 1 || length == 2 || length == 4) {
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < length; ++i) v |= std::uint32_t{bytes[i]} << (8 * i);
        out["value"] = v;  // little-endian
    }
    finish(r, std::move(out));
}

void Controller::h_read_named(const Request& r) {
    if (symbols_.empty()) throw std::runtime_error("no symbols for this game (docs/dev/GAME-CONFIG.md \"[symbols]\")");
    Json out = Json::object();
    const Json* names = param(r.params, "names");
    if (names && names->is_array()) {
        Json values = Json::object();
        for (const Json& n : names->as_array()) {
            if (!n.is_string()) throw ParamError("names must be strings");
            try {
                values[n.as_string()] = symbols_.read(n.as_string(), peeker());
            } catch (const std::exception& e) {
                values[n.as_string()] = Json(Json::Object{{"error", e.what()}});
            }
        }
        out["values"] = std::move(values);
    } else {
        const std::string name = str_param(r.params, "name");
        out["name"] = name;
        out["value"] = symbols_.read(name, peeker());
    }
    out["frame"] = sys_.frames();
    finish(r, std::move(out));
}

void Controller::h_list_symbols(const Request& r) { finish(r, Json(Json::Object{{"symbols", symbols_.describe(peeker())}})); }

void Controller::h_list_states(const Request& r) {
    const auto dir = hooks_.state_dir ? hooks_.state_dir() : std::nullopt;
    Json list = Json::array();
    if (dir) {
        const std::string stem = hooks_.state_stem();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(*dir, ec)) {
            const std::string name = e.path().filename().string();
            if (!name.starts_with(stem + ".")) continue;
            const std::string rest = name.substr(stem.size() + 1);
            Json item = Json::object();
            if (rest.size() == 3 && rest.starts_with("ss") && rest[2] >= '1' && rest[2] <= '9') {
                item["slot"] = rest[2] - '0';
            } else if (rest.starts_with("state-") && rest.size() > 6) {
                item["name"] = rest.substr(6);
            } else {
                continue;
            }
            item["bytes"] = static_cast<std::uint64_t>(e.file_size(ec));
            list.push_back(std::move(item));
        }
    }
    finish(r, Json(Json::Object{{"states", std::move(list)}, {"dir", dir ? dir->string() : std::string()}}));
}

void Controller::h_list_cheats(const Request& r) {
    const std::string filter = str_param(r.params, "filter", "");
    const auto limit = static_cast<std::size_t>(int_param(r.params, "limit", 200, 1, 5000));
    Json list = Json::array();
    std::size_t total = 0;
    const auto& all = cheats_.cheats();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto& c = all[i];
        if (!filter.empty()) {
            std::string a = c.name, b = filter;
            for (auto* s : {&a, &b}) std::transform(s->begin(), s->end(), s->begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (a.find(b) == std::string::npos) continue;
        }
        if (++total > limit) continue;
        Json item = Json::object();
        item["index"] = i;
        item["name"] = c.name;
        item["enabled"] = c.enabled;
        if (c.custom) item["custom"] = true;
        if (c.param) {
            item["value"] = c.value;
            item["value_label"] = c.param->label;
            item["value_text"] = c.param->describe(c.value);
            if (c.param->choices.empty()) {
                item["min"] = c.param->min;
                item["max"] = c.param->max;
            } else {
                item["choices"] = c.param->choices.size();
            }
        }
        if (!c.note.empty()) item["note"] = c.note;
        list.push_back(std::move(item));
    }
    finish(r, Json(Json::Object{{"cheats", std::move(list)}, {"matching", total}}));
}

// ---------------------------------------------------------------- play

void Controller::schedule_input(const Request& r, const std::vector<std::pair<std::uint16_t, std::uint32_t>>& steps) {
    const std::uint64_t now = sys_.frames();
    std::uint64_t at = now + 1;  // keys are applied from the next frame on
    for (const auto& [mask, frames] : steps) {
        if (mask) timed_.push_back({mask, at, at + frames});
        at += frames;
    }
    const std::uint64_t until = at - 1;
    if (paused_) run_until_ = std::max(run_until_, until);
    Json result = Json::object();
    result["from_frame"] = now + 1;
    result["frames"] = until - now;
    if (!bool_param(r.params, "wait", true)) {
        result["frame"] = now;
        finish(r, std::move(result));
        return;
    }
    pending_.push_back({r, now + 1, until, std::move(result)});
}

void Controller::h_press(const Request& r) {
    const std::uint16_t mask = parse_buttons(param(r.params, "buttons") ? *param(r.params, "buttons") : Json());
    if (!mask) throw ParamError("press needs buttons");
    const auto frames = static_cast<std::uint32_t>(int_param(r.params, "frames", 6, 1, kMaxFrames));
    // An optional release time after the press, so presses in a row register as separate.
    const auto after = static_cast<std::uint32_t>(int_param(r.params, "release_frames", 0, 0, kMaxFrames));
    std::vector<std::pair<std::uint16_t, std::uint32_t>> steps = {{mask, frames}};
    if (after) steps.emplace_back(0, after);
    schedule_input(r, steps);
}

void Controller::h_hold(const Request& r) {
    const std::uint16_t mask = parse_buttons(param(r.params, "buttons") ? *param(r.params, "buttons") : Json());
    if (!mask) throw ParamError("hold needs buttons");
    hold_ = static_cast<std::uint16_t>(hold_ | mask);
    finish(r, Json(Json::Object{{"held", button_names(hold_)}, {"frame", sys_.frames()}}));
}

void Controller::h_release(const Request& r) {
    const Json* b = param(r.params, "buttons");
    const std::uint16_t mask = b ? parse_buttons(*b) : 0x3FF;
    hold_ = static_cast<std::uint16_t>(hold_ & ~mask);
    for (Timed& t : timed_) t.mask = static_cast<std::uint16_t>(t.mask & ~mask);
    finish(r, Json(Json::Object{{"held", button_names(hold_)}, {"frame", sys_.frames()}}));
}

void Controller::h_input_sequence(const Request& r) {
    const Json* steps_v = param(r.params, "steps");
    if (!steps_v || !steps_v->is_array() || steps_v->size() == 0 || steps_v->size() > 1000) {
        throw ParamError("steps must be a list of 1 to 1000 {buttons, frames} items");
    }
    std::vector<std::pair<std::uint16_t, std::uint32_t>> steps;
    std::uint64_t total = 0;
    for (const Json& s : steps_v->as_array()) {
        if (!s.is_object()) throw ParamError("each step is an object {buttons, frames}");
        const std::uint16_t mask = parse_buttons(param(s, "buttons") ? *param(s, "buttons") : Json());
        const auto frames = static_cast<std::uint32_t>(int_param(s, "frames", 1, 1, kMaxFrames));
        total += frames;
        steps.emplace_back(mask, frames);
    }
    if (total > kMaxFrames) throw ParamError("a sequence may last at most " + std::to_string(kMaxFrames) + " frames");
    schedule_input(r, steps);
}

void Controller::h_step_frames(const Request& r) {
    const auto frames = static_cast<std::uint64_t>(int_param(r.params, "frames", 1, 1, kMaxFrames));
    const std::uint64_t until = sys_.frames() + frames;
    if (paused_) run_until_ = std::max(run_until_, until);
    pending_.push_back({r, sys_.frames() + 1, until, Json::object()});
}

void Controller::h_pause(const Request& r) {
    paused_ = true;
    run_until_ = 0;
    stop_reason_ = Json(Json::Object{{"reason", "pause"}});
    finish(r, Json(Json::Object{{"paused", true}, {"frame", sys_.frames()}}));
}

void Controller::h_resume(const Request& r) {
    paused_ = false;
    stop_reason_ = Json();
    finish(r, Json(Json::Object{{"paused", false}, {"frame", sys_.frames()}}));
}

void Controller::h_set_speed(const Request& r) {
    const int percent = static_cast<int>(int_param(r.params, "percent", std::nullopt, 0, 1000));
    if (!player::valid_speed(percent)) throw ParamError("percent must be 100, 200, 300 or 400");
    if (hooks_.set_speed) hooks_.set_speed(percent);
    finish(r, Json(Json::Object{{"speed", percent}}));
}

// ---------------------------------------------------------------- control

fs::path Controller::state_file(const Json& params) const {
    const auto dir = hooks_.state_dir ? hooks_.state_dir() : std::nullopt;
    if (!dir) throw std::runtime_error("no folder for save states; start the game with --save-dir");
    const std::string stem = hooks_.state_stem();
    if (const Json* name = param(params, "name")) {
        if (!name->is_string() || !valid_state_name(name->as_string())) {
            throw ParamError("name must be 1-64 letters, digits, '-' or '_'");
        }
        return *dir / (stem + ".state-" + name->as_string());
    }
    const auto slot = int_param(params, "slot", 1, 1, 9);
    return *dir / (stem + ".ss" + std::to_string(slot));
}

void Controller::h_save_state(const Request& r) {
    const fs::path path = state_file(r.params);
    at_safe_point(r, [this, path](System& s) {
        const auto bytes = s.save_state();
        write_file_atomic(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        log("saved state " + path.filename().string());
        if (journal_on()) journal_->thumbnail(s.frames(), s.framebuffer(), "save_state");
        Json out = Json::object();
        out["file"] = path.string();
        out["bytes"] = bytes.size();
        out["frame"] = s.frames();
        return out;
    });
}

void Controller::h_load_state(const Request& r) {
    const fs::path path = state_file(r.params);
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("no save state at " + path.string());
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    at_safe_point(r, [this, path, bytes](System& s) {
        s.load_state(*bytes);
        if (hooks_.after_load) hooks_.after_load();
        // The frame counter went back: frame targets and queued input refer to the old timeline.
        for (Pending& p : pending_) {
            Json result = std::move(p.result);
            result["frame"] = s.frames();
            result["interrupted"] = Json(Json::Object{{"reason", "load_state"}});
            finish(p.request, std::move(result));
        }
        pending_.clear();
        timed_.clear();
        run_until_ = 0;
        takeover_until_ = 0;
        log("loaded state " + path.filename().string());
        if (journal_on()) journal_->thumbnail(s.frames(), s.framebuffer(), "load_state");
        Json out = Json::object();
        out["file"] = path.string();
        out["frame"] = s.frames();
        out["frame_hash"] = frame_hash(s);
        return out;
    });
}

void Controller::h_set_cheat(const Request& r) {
    Json out = Json::object();
    std::size_t index = 0;
    if (const Json* code = param(r.params, "code")) {
        std::vector<std::string> lines;
        if (code->is_string()) {
            lines = cheats::split_lines(code->as_string());
        } else if (code->is_array()) {
            for (const Json& l : code->as_array()) {
                if (!l.is_string()) throw ParamError("code lines must be strings");
                lines.push_back(l.as_string());
            }
        }
        if (lines.empty()) throw ParamError("code must be a string or a list of lines");
        const auto format = cheats::parse_format(str_param(r.params, "format", "auto"));
        if (!format) throw ParamError("format must be auto, raw, gsv1, gsv1raw, arv3, arv3raw or cb");
        const std::string name = str_param(r.params, "name", "Agent code " + std::to_string(cheats_.cheats().size() + 1));
        index = cheats_.add_custom(name, *format, lines, bool_param(r.params, "persistent", false));
        out["added"] = true;
    } else {
        if (const Json* i = param(r.params, "index")) {
            index = static_cast<std::size_t>(integer(*i, "index"));
        } else {
            const std::string name = str_param(r.params, "name");
            const auto& all = cheats_.cheats();
            const auto it = std::find_if(all.begin(), all.end(), [&](const cheats::Cheat& c) { return c.name == name; });
            if (it == all.end()) throw std::runtime_error("no cheat named '" + name + "' (list_cheats shows them)");
            index = static_cast<std::size_t>(it - all.begin());
        }
        if (index >= cheats_.cheats().size()) throw ParamError("index is past the cheat list");
        if (const Json* v = param(r.params, "value")) {
            if (const auto error = cheats_.set_value(index, static_cast<std::uint32_t>(integer(*v, "value")))) {
                throw std::runtime_error(*error);
            }
        }
        if (param(r.params, "enabled")) {
            if (const auto error = cheats_.set_enabled(index, bool_param(r.params, "enabled", std::nullopt))) {
                throw std::runtime_error(*error);
            }
        }
    }
    if (hooks_.cheats_changed) hooks_.cheats_changed();
    const auto& c = cheats_.cheats()[index];
    out["index"] = index;
    out["name"] = c.name;
    out["enabled"] = c.enabled;
    if (c.param) out["value"] = c.value;
    log(std::string("cheat ") + (c.enabled ? "on: " : "off: ") + c.name);
    finish(r, std::move(out));
}

void Controller::h_write_memory(const Request& r) {
    const std::uint32_t addr = addr_param(r.params);
    std::vector<std::uint8_t> bytes;
    if (const Json* hex = param(r.params, "hex")) {
        if (!hex->is_string()) throw ParamError("hex must be a string of hex digits");
        try {
            bytes = hex_decode(hex->as_string());
        } catch (const JsonError& e) {
            throw ParamError(e.what());
        }
        if (bytes.empty() || bytes.size() > 4096) throw ParamError("hex must hold 1 to 4096 bytes");
    } else {
        const auto size = int_param(r.params, "size", 1, 1, 4);
        if (size == 3) throw ParamError("size must be 1, 2 or 4");
        const auto value = static_cast<std::uint32_t>(int_param(r.params, "value", std::nullopt, -2147483648ll, 0xFFFFFFFFll));
        for (int i = 0; i < size; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    const auto last = addr + static_cast<std::uint32_t>(bytes.size() - 1);
    if (!writable_region(addr) || !writable_region(last) || (addr >> 24) != (last >> 24)) {
        throw std::runtime_error("only EWRAM, IWRAM, I/O, palette, VRAM and OAM are writable (" + region_name(addr) +
                                 "); ROM changes are cheats (set_cheat)");
    }
    std::vector<std::uint8_t> before(bytes.size());
    peek(sys_, addr, before);
    if (bytes.size() == 2 && (addr & 1) == 0) {
        sys_.write16(addr, bytes[0] | (std::uint32_t{bytes[1]} << 8));
    } else if (bytes.size() == 4 && (addr & 3) == 0) {
        sys_.write32(addr, bytes[0] | (std::uint32_t{bytes[1]} << 8) | (std::uint32_t{bytes[2]} << 16) | (std::uint32_t{bytes[3]} << 24));
    } else {
        for (std::size_t i = 0; i < bytes.size(); ++i) sys_.write8(addr + static_cast<std::uint32_t>(i), bytes[i]);
    }
    log("memory write " + hex32(addr) + " (" + std::to_string(bytes.size()) + " bytes)");
    Json out = Json::object();
    out["address"] = hex32(addr);
    out["bytes"] = bytes.size();
    out["before"] = hex_encode(before.data(), before.size());
    out["after"] = hex_encode(bytes.data(), bytes.size());
    finish(r, std::move(out));
}

void Controller::h_journal_clear(const Request& r) {
    if (!journal_) throw std::runtime_error("no journal folder");
    const std::string scope = str_param(r.params, "scope", "game");
    if (scope != "game" && scope != "all") throw ParamError("scope must be game or all");
    const fs::path keep = journal_on() ? journal_->session_dir() : fs::path();
    const std::size_t n = journal::clear(journal_->root(), scope == "all" ? std::string() : journal_->game(), keep);
    log("journal: cleared " + std::to_string(n) + " sessions");
    finish(r, Json(Json::Object{{"deleted_sessions", n}}));
}

void Controller::h_quit(const Request& r) {
    if (!hooks_.quit) throw std::runtime_error("this player cannot quit on request");
    finish(r, Json(Json::Object{{"quitting", true}, {"frame", sys_.frames()}}));
    log("quit requested by " + r.client);
    paused_ = false;
    hooks_.quit();
}

// ---------------------------------------------------------------- debug

void Controller::h_get_registers(const Request& r) {
    const anygba_cpu& c = sys_.cpu();
    Json regs = Json::object();
    for (int i = 0; i < 16; ++i) regs["r" + std::to_string(i)] = hex32(c.r[i]);
    regs["cpsr"] = hex32(ag_cpsr(&c));
    regs["spsr"] = hex32(c.spsr);
    static constexpr const char* kModes[] = {"usr", "fiq", "irq", "svc", "?", "?", "?", "abt", "?", "?", "?", "und", "?", "?", "?", "sys"};
    regs["mode"] = kModes[c.ctrl & 0xF];
    regs["thumb"] = c.t != 0;
    regs["flags"] = std::string(c.n ? "N" : "-") + (c.z ? "Z" : "-") + (c.c ? "C" : "-") + (c.v ? "V" : "-");
    regs["irq_disabled"] = (c.ctrl & 0x80) != 0;
    regs["frame"] = sys_.frames();
    regs["note"] = "r15 is the next instruction at frame boundaries and stops; inside compiled code it is the last value the code stored";
    finish(r, std::move(regs));
}

void Controller::h_disassemble(const Request& r) {
    const anygba_cpu& c = sys_.cpu();
    std::uint32_t addr = param(r.params, "address") ? addr_param(r.params) : c.r[15];
    const auto count = int_param(r.params, "count", 20, 1, 200);
    const std::string mode = str_param(r.params, "mode", "auto");
    if (mode != "auto" && mode != "arm" && mode != "thumb") throw ParamError("mode must be auto, arm or thumb");
    const bool thumb = mode == "thumb" || (mode == "auto" && (param(r.params, "address") ? (addr & 1) != 0 : c.t != 0));
    addr &= thumb ? ~1u : ~3u;
    std::set<std::uint32_t> entries;
    if (const anygba_game* g = sys_.game()) {
        for (std::uint32_t i = 0; i < g->function_count; ++i) entries.insert(g->functions[i].addr);
    }
    Json lines = Json::array();
    for (std::int64_t i = 0; i < count; ++i) {
        decode::Insn in;
        std::string raw;
        if (thumb) {
            const auto h = static_cast<std::uint16_t>(peek_le(sys_, addr, 2));
            std::uint16_t next = 0;
            try {
                next = static_cast<std::uint16_t>(peek_le(sys_, addr + 2, 2));
            } catch (const std::exception&) {
            }
            in = decode::decode_thumb_at(h, next, addr);
        } else {
            in = decode::decode_arm(peek_le(sys_, addr, 4), addr);
        }
        std::vector<std::uint8_t> b(in.size);
        peek(sys_, addr, b);
        Json line = Json::object();
        line["address"] = hex32(addr);
        line["bytes"] = hex_encode(b.data(), b.size());
        line["text"] = decode::disassemble(in);
        if (entries.contains(addr)) line["compiled_function"] = true;
        if (addr == c.r[15]) line["pc"] = true;
        if (sys_.breakpoints().contains(addr)) line["breakpoint"] = true;
        lines.push_back(std::move(line));
        addr = in.next();
    }
    finish(r, Json(Json::Object{{"mode", thumb ? "thumb" : "arm"}, {"lines", std::move(lines)}}));
}

void Controller::sync_watches() {
    std::uint16_t mask = 0;
    for (const auto& [id, w] : watches_) {
        for (std::uint32_t a = w.addr >> 24; a <= (w.addr + w.size - 1) >> 24 && a < 16; ++a) mask = static_cast<std::uint16_t>(mask | (1u << a));
    }
    sys_.set_watched_regions(mask);
}

void Controller::sync_breakpoints() {
    std::set<std::uint32_t> addrs;
    for (const auto& [id, b] : breakpoints_) addrs.insert(b.addr);
    sys_.set_breakpoints(std::move(addrs));
}

void Controller::on_access(const System::Access& a) {
    for (auto& [id, w] : watches_) {
        if (a.addr + a.size <= w.addr || a.addr >= w.addr + w.size) continue;
        bool hit = false;
        if (w.kind == "read") {
            hit = !a.write;
        } else if (a.write) {
            if (w.kind == "write") {
                hit = true;
            } else {
                std::vector<std::uint8_t> now(w.size);
                peek(sys_, w.addr, now);
                if (w.kind == "change") {
                    hit = now != w.shadow;
                } else {  // equals
                    std::uint32_t v = 0;
                    for (std::uint32_t i = 0; i < w.size && i < 4; ++i) v |= std::uint32_t{now[i]} << (8 * i);
                    hit = v == w.value && now != w.shadow;
                }
                w.shadow = std::move(now);
            }
        }
        if (!hit || servicing_) continue;
        ++w.hits;
        Json reason = Json::object();
        reason["reason"] = "watch";
        reason["id"] = id;
        reason["access"] = a.write ? "write" : "read";
        reason["address"] = hex32(a.addr);
        reason["size"] = a.size;
        reason["value"] = hex32(a.value);
        reason["pc_hint"] = hex32(sys_.cpu().r[15]);
        log("watch " + std::to_string(id) + ": " + (a.write ? "write " : "read ") + hex32(a.addr) + " = " + hex32(a.value) +
            (w.stop ? " (stopped)" : ""));
        if (w.stop) {
            stop_here(std::move(reason));
            return;
        }
    }
}

void Controller::h_watch_memory(const Request& r) {
    Watch w;
    w.addr = addr_param(r.params);
    w.size = static_cast<std::uint32_t>(int_param(r.params, "length", 4, 1, 256));
    w.kind = str_param(r.params, "on", "write");
    if (w.kind != "write" && w.kind != "read" && w.kind != "change" && w.kind != "equals") {
        throw ParamError("on must be write, read, change or equals");
    }
    if (w.kind == "equals") {
        w.value = static_cast<std::uint32_t>(int_param(r.params, "value", std::nullopt, 0, 0xFFFFFFFFll));
        if (w.size > 4) throw ParamError("equals watches 1 to 4 bytes");
    }
    const std::string action = str_param(r.params, "action", "stop");
    if (action != "stop" && action != "notify") throw ParamError("action must be stop or notify");
    w.stop = action == "stop";
    if (settings_.readonly && w.stop) throw std::runtime_error("read-only agent mode: watches can only notify");
    if (region_name(w.addr) == "unmapped" || region_name(w.addr) == "rom" || region_name(w.addr) == "bios") {
        throw ParamError("watches cover RAM, I/O, palette, VRAM and OAM");
    }
    w.shadow.resize(w.size);
    peek(sys_, w.addr, w.shadow);
    w.id = next_debug_id_++;
    const int id = w.id;
    watches_[id] = std::move(w);
    sync_watches();
    finish(r, Json(Json::Object{{"id", id}, {"note", "tail_log and list_debug show hits; a stop pauses the game (resume continues)"}}));
}

void Controller::h_break_at_function(const Request& r) {
    const std::uint32_t addr = addr_param(r.params) & ~1u;
    const std::string action = str_param(r.params, "action", "stop");
    if (action != "stop" && action != "notify") throw ParamError("action must be stop or notify");
    if (settings_.readonly && action == "stop") throw std::runtime_error("read-only agent mode: breakpoints can only notify");
    std::string kind = "interpreted";
    if (const anygba_game* g = sys_.game()) {
        for (std::uint32_t i = 0; i < g->function_count; ++i) {
            const anygba_function_entry& f = g->functions[i];
            if (f.addr == addr) {
                kind = f.addr >= 0x08000000 && f.addr < 0x0E000000 ? "compiled_entry" : "compiled_ram_entry";
                break;
            }
            if (f.code_end > f.code_begin && addr > f.addr && addr >= f.code_begin && addr < f.code_end) {
                throw std::runtime_error(hex32(addr) + " is inside the compiled function at " + hex32(f.addr) +
                                         "; compiled code can only stop at function entries");
            }
        }
    }
    if (kind == "compiled_ram_entry") {
        throw std::runtime_error(hex32(addr) + " is a compiled RAM function; breakpoints cover ROM function entries and interpreted code");
    }
    Breakpoint b;
    b.id = next_debug_id_++;
    b.addr = addr;
    b.stop = action == "stop";
    const int id = b.id;
    breakpoints_[id] = b;
    sync_breakpoints();
    finish(r, Json(Json::Object{{"id", id}, {"address", hex32(addr)}, {"kind", kind}}));
}

void Controller::h_clear_debug(const Request& r) {
    const Json* id_v = param(r.params, "id");
    std::size_t removed = 0;
    if (!id_v || (id_v->is_string() && id_v->as_string() == "all")) {
        removed = breakpoints_.size() + watches_.size();
        breakpoints_.clear();
        watches_.clear();
    } else {
        const auto id = static_cast<int>(integer(*id_v, "id"));
        removed = breakpoints_.erase(id) + watches_.erase(id);
        if (!removed) throw ParamError("no breakpoint or watch " + std::to_string(id));
    }
    sync_breakpoints();
    sync_watches();
    finish(r, Json(Json::Object{{"removed", removed}}));
}

void Controller::h_list_debug(const Request& r) {
    Json bps = Json::array(), ws = Json::array();
    for (const auto& [id, b] : breakpoints_) {
        bps.push_back(Json(Json::Object{{"id", id}, {"address", hex32(b.addr)}, {"action", b.stop ? "stop" : "notify"}, {"hits", b.hits}}));
    }
    for (const auto& [id, w] : watches_) {
        ws.push_back(Json(Json::Object{{"id", id}, {"address", hex32(w.addr)}, {"length", w.size}, {"on", w.kind},
                                       {"action", w.stop ? "stop" : "notify"}, {"hits", w.hits}}));
    }
    finish(r, Json(Json::Object{{"breakpoints", std::move(bps)}, {"watches", std::move(ws)}, {"stop_reason", stop_reason_}}));
}

void Controller::h_get_call_stack(const Request& r) {
    const anygba_cpu& c = sys_.cpu();
    Json frames = Json::array();
    for (std::uint32_t i = c.ret_depth; i-- > 0;) frames.push_back(hex32(c.ret_stack[i]));
    Json out = Json::object();
    out["pc"] = hex32(c.r[15]);
    out["lr"] = hex32(c.r[14]);
    out["sp"] = hex32(c.r[13]);
    out["return_addresses"] = std::move(frames);
    out["note"] = "return addresses of active compiled call sites, innermost first; empty at frame boundaries after a dispatch";
    finish(r, std::move(out));
}

void Controller::h_get_miss_log(const Request& r) {
    const auto limit = static_cast<std::size_t>(int_param(r.params, "limit", 500, 1, 100000));
    Json addrs = Json::array();
    for (const std::uint32_t a : sys_.misses()) {
        if (addrs.size() >= limit) break;
        addrs.push_back(hex32(a));
    }
    Json out = Json::object();
    out["count"] = sys_.misses().size();
    out["addresses"] = std::move(addrs);
    out["file"] = sys_.options().log_misses ? sys_.options().miss_log.string() : std::string();
    finish(r, std::move(out));
}

void Controller::h_tail_log(const Request& r) {
    const auto n = static_cast<std::size_t>(int_param(r.params, "lines", 50, 1, static_cast<std::int64_t>(kLogLines)));
    Json lines = Json::array();
    for (std::size_t i = log_.size() > n ? log_.size() - n : 0; i < log_.size(); ++i) lines.push_back(log_[i]);
    finish(r, Json(Json::Object{{"lines", std::move(lines)}}));
}

// ---------------------------------------------------------------- journal

void Controller::h_journal_note(const Request& r) {
    if (!journal_on()) throw std::runtime_error("the journal is off; turn it on with set_journal");
    const std::string text = str_param(r.params, "text");
    if (text.empty() || text.size() > 4000) throw ParamError("text must be 1 to 4000 characters");
    const std::string kind = str_param(r.params, "kind", "note");
    if (kind != "note" && kind != "goal" && kind != "summary") throw ParamError("kind must be note, goal or summary");
    journal_->record(sys_.frames(), kind, "agent", Json(Json::Object{{"text", text}, {"client", r.client}}));
    server_.reply(r, Json(Json::Object{{"recorded", kind}, {"frame", sys_.frames()}}));  // the note is its own record
}

void Controller::h_journal_summary(const Request& r) {
    if (!journal_) throw std::runtime_error("no journal folder");
    fs::path dir;
    if (const Json* s = param(r.params, "session")) {
        if (!s->is_string() || s->as_string().find_first_of("/\\.") != std::string::npos) throw ParamError("bad session id");
        dir = journal_->root() / journal_->game() / s->as_string();
    } else if (journal_on()) {
        dir = journal_->session_dir();
    } else {
        const auto sessions = journal::list_sessions(journal_->root(), journal_->game());
        if (sessions.empty()) throw std::runtime_error("no journal sessions for this game yet");
        dir = sessions.front().dir;
    }
    finish(r, journal::summarize(dir));
}

void Controller::h_journal_search(const Request& r) {
    if (!journal_) throw std::runtime_error("no journal folder");
    journal::Query q;
    q.text = str_param(r.params, "text", "");
    q.kind = str_param(r.params, "kind", "");
    q.who = str_param(r.params, "who", "");
    q.method = str_param(r.params, "method", "");
    q.session = str_param(r.params, "session", "");
    q.game = str_param(r.params, "game", "this") == "all" ? std::string() : journal_->game();
    q.limit = static_cast<std::size_t>(int_param(r.params, "limit", 50, 1, 500));
    finish(r, Json(Json::Object{{"records", journal::search(journal_->root(), q)}}));
}

void Controller::h_journal_export(const Request& r) {
    if (!journal_) throw std::runtime_error("no journal folder");
    const fs::path to = fs::path(str_param(r.params, "to"));
    if (!to.is_absolute()) throw ParamError("to must be an absolute folder path");
    fs::path dir = journal_on() ? journal_->session_dir() : fs::path();
    if (const Json* s = param(r.params, "session")) {
        if (!s->is_string() || s->as_string().find_first_of("/\\.") != std::string::npos) throw ParamError("bad session id");
        dir = journal_->root() / journal_->game() / s->as_string();
    }
    if (dir.empty()) throw ParamError("no active session; pass session");
    finish(r, Json(Json::Object{{"exported_to", journal::export_session(dir, to).string()}}));
}

}  // namespace anygba::control
