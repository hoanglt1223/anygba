#include "journal/journal.h"

#include "anygba/system.hpp"
#include "control/png.h"
#include "control/server.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <system_error>

namespace anygba::journal {

namespace fs = std::filesystem;

namespace {

constexpr const char* kFile = "session.jsonl";

std::tm utc(std::time_t t) {
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

std::string now_iso() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::tm tm = utc(t);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

std::string new_session_id() {
    const std::tm tm = utc(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d%02d%02dT%02d%02d%02dZ-%d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, control::current_pid());
    return buf;
}

std::uint64_t frames_for(int seconds) {
    return static_cast<std::uint64_t>(seconds * kFramesPerSecond + 0.5);
}

std::uint64_t dir_size(const fs::path& dir) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) total += e.file_size(ec);
    }
    return total;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

template <typename Fn>
void each_record(const fs::path& file, Fn&& fn) {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            if (!fn(line, Json::parse(line))) return;
        } catch (const control::JsonError&) {
            // A line cut short by a crash; skip it.
        }
    }
}

std::string str_of(const Json& record, std::string_view key) {
    const Json* v = record.find(key);
    return v && v->is_string() ? v->as_string() : std::string();
}

}  // namespace

Journal::Journal(fs::path root, std::string game) : root_(std::move(root)), game_(std::move(game)) {}

Journal::~Journal() {
    if (active()) out_.close();
}

void Journal::start(std::uint64_t frame, Json info) {
    if (active()) return;
    const std::string base = new_session_id();
    session_ = base;
    std::error_code ec;
    // Switching the journal off and on within a second must still start a new session.
    for (int n = 2; fs::exists(session_dir(), ec); ++n) session_ = base + "-" + std::to_string(n);
    const fs::path dir = session_dir();
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error("cannot create the journal folder " + dir.string() + ": " + ec.message());
    apply_retention(root_, settings_.max_mb, settings_.max_days, dir);
    out_.open(dir / kFile, std::ios::binary | std::ios::app);
    if (!out_) throw std::runtime_error("cannot write " + (dir / kFile).string());
    seq_ = 0;
    start_frame_ = frame;
    next_snapshot_ = frame;
    next_thumbnail_ = frame;
    Json data = info.is_object() ? std::move(info) : Json::object();
    Json s = Json::object();
    s["snapshot_seconds"] = settings_.snapshot_seconds;
    s["thumbnail_seconds"] = settings_.thumbnail_seconds;
    s["max_mb"] = settings_.max_mb;
    s["max_days"] = settings_.max_days;
    data["settings"] = std::move(s);
    record(frame, "session_start", "user", std::move(data));
}

void Journal::stop(std::uint64_t frame) {
    if (!active()) return;
    out_.flush();
    Json data = Json::object();
    data["summary"] = summarize(session_dir());
    data["frames_played"] = frame >= start_frame_ ? frame - start_frame_ : 0;
    record(frame, "session_end", "user", std::move(data));
    out_.close();
}

void Journal::record(std::uint64_t frame, std::string_view kind, std::string_view who, Json data) {
    if (!active()) return;
    Json r = Json::object();
    r["seq"] = ++seq_;
    r["time"] = now_iso();
    r["frame"] = frame;
    r["kind"] = kind;
    r["who"] = who;
    if (data.is_object()) {
        for (auto& [k, v] : data.as_object()) r[k] = std::move(v);
    }
    out_ << r.dump() << '\n';
    out_.flush();  // every record reaches the disk, even if the game crashes next
}

void Journal::thumbnail(std::uint64_t frame, std::span<const std::uint16_t> picture, std::string_view reason) {
    if (!active()) return;
    const fs::path dir = session_dir() / "thumbs";
    const std::string name = "f" + std::to_string(frame) + ".png";
    try {
        std::error_code ec;
        fs::create_directories(dir, ec);
        const auto png = control::encode_png(picture, kScreenWidth, kScreenHeight, 0.5);
        control::write_file_atomic(dir / name, std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
        Json data = Json::object();
        data["file"] = "thumbs/" + name;
        data["reason"] = reason;
        record(frame, "thumbnail", "user", std::move(data));
    } catch (const std::exception& e) {
        record(frame, "error", "user", Json(Json::Object{{"message", std::string("thumbnail: ") + e.what()}}));
    }
}

void Journal::on_frame(std::uint64_t frame, const std::function<Json()>& snapshot, std::span<const std::uint16_t> picture) {
    if (!active()) return;
    if (settings_.snapshot_seconds > 0 && frame >= next_snapshot_) {
        next_snapshot_ = frame + frames_for(settings_.snapshot_seconds);
        Json values = snapshot ? snapshot() : Json::object();
        if (values.size() > 0) record(frame, "snapshot", "user", Json(Json::Object{{"symbols", std::move(values)}}));
    }
    if (settings_.thumbnail_seconds > 0 && frame >= next_thumbnail_) {
        next_thumbnail_ = frame + frames_for(settings_.thumbnail_seconds);
        thumbnail(frame, picture, "interval");
    }
}

std::vector<SessionRef> list_sessions(const fs::path& root, std::string_view game) {
    std::vector<std::pair<fs::file_time_type, SessionRef>> found;
    std::error_code ec;
    for (const auto& g : fs::directory_iterator(root, ec)) {
        if (!g.is_directory(ec)) continue;
        const std::string name = g.path().filename().string();
        if (!game.empty() && name != game) continue;
        for (const auto& s : fs::directory_iterator(g.path(), ec)) {
            const fs::path file = s.path() / kFile;
            if (!s.is_directory(ec) || !fs::exists(file, ec)) continue;
            SessionRef ref{name, s.path().filename().string(), s.path(), dir_size(s.path())};
            found.emplace_back(fs::last_write_time(file, ec), std::move(ref));
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second.id > b.second.id;
    });
    std::vector<SessionRef> out;
    for (auto& [t, ref] : found) out.push_back(std::move(ref));
    return out;
}

Json search(const fs::path& root, const Query& q) {
    Json out = Json::array();
    const std::string needle = lower(q.text);
    const std::size_t limit = std::clamp<std::size_t>(q.limit, 1, 500);
    for (const SessionRef& s : list_sessions(root, q.game)) {
        if (!q.session.empty() && s.id != q.session) continue;
        std::vector<Json> hits;
        each_record(s.dir / kFile, [&](const std::string& line, Json record) {
            if (!q.kind.empty() && str_of(record, "kind") != q.kind) return true;
            if (!q.who.empty() && str_of(record, "who") != q.who) return true;
            if (!q.method.empty() && str_of(record, "method") != q.method) return true;
            if (!needle.empty() && lower(line).find(needle) == std::string::npos) return true;
            record["game"] = s.game;
            record["session"] = s.id;
            hits.push_back(std::move(record));
            return true;
        });
        for (auto it = hits.rbegin(); it != hits.rend() && out.size() < limit; ++it) out.push_back(std::move(*it));
        if (out.size() >= limit) break;
    }
    return out;
}

Json summarize(const fs::path& session_dir) {
    std::map<std::string, std::int64_t> kinds, methods, who;
    Json notes = Json::array(), goals = Json::array(), errors = Json::array(), agent_summaries = Json::array();
    Json last_snapshot, info;
    std::string first_time, last_time;
    std::int64_t first_frame = -1, last_frame = 0, records = 0;
    each_record(session_dir / kFile, [&](const std::string&, Json r) {
        ++records;
        const std::string kind = str_of(r, "kind");
        ++kinds[kind];
        ++who[str_of(r, "who")];
        const Json* frame = r.find("frame");
        if (frame && frame->is_int()) {
            if (first_frame < 0) first_frame = frame->as_int();
            last_frame = frame->as_int();
        }
        if (first_time.empty()) first_time = str_of(r, "time");
        last_time = str_of(r, "time");
        const auto keep_last = [](Json& list, Json item, std::size_t n) {
            list.push_back(std::move(item));
            if (list.size() > n) list.as_array().erase(list.as_array().begin());
        };
        const auto brief = [&r](std::string_view key) {
            Json b = Json::object();
            b["frame"] = r.find("frame") ? *r.find("frame") : Json();
            b[key] = r.find(key) ? *r.find(key) : Json();
            return b;
        };
        if (kind == "command") {
            ++methods[str_of(r, "method")];
            if (r.find("error")) keep_last(errors, brief("error"), 10);
        } else if (kind == "note") {
            keep_last(notes, brief("text"), 20);
        } else if (kind == "goal") {
            keep_last(goals, brief("text"), 20);
        } else if (kind == "error") {
            keep_last(errors, brief("message"), 10);
        } else if (kind == "summary") {
            keep_last(agent_summaries, brief("text"), 5);
        } else if (kind == "snapshot") {
            last_snapshot = r.find("symbols") ? *r.find("symbols") : Json();
        } else if (kind == "session_start") {
            info = r;
        }
        return true;
    });
    const auto to_json = [](const std::map<std::string, std::int64_t>& m) {
        Json o = Json::object();
        for (const auto& [k, v] : m) o[k.empty() ? "?" : k] = v;
        return o;
    };
    Json s = Json::object();
    s["session"] = session_dir.filename().string();
    s["records"] = records;
    s["first_frame"] = first_frame < 0 ? 0 : first_frame;
    s["last_frame"] = last_frame;
    s["started"] = first_time;
    s["last_record"] = last_time;
    s["ended_cleanly"] = kinds.contains("session_end");
    s["kinds"] = to_json(kinds);
    s["who"] = to_json(who);
    s["commands"] = to_json(methods);
    s["state_saves"] = methods["save_state"] + kinds["state_save"];
    s["state_loads"] = methods["load_state"] + kinds["state_load"];
    s["memory_writes"] = methods["write_memory"];
    s["cheat_changes"] = methods["set_cheat"] + kinds["cheats"];
    s["notes"] = std::move(notes);
    s["goals"] = std::move(goals);
    s["agent_summaries"] = std::move(agent_summaries);
    s["errors"] = std::move(errors);
    if (!last_snapshot.is_null()) s["last_snapshot"] = std::move(last_snapshot);
    if (const Json* title = info.find("title")) s["title"] = *title;
    return s;
}

std::size_t apply_retention(const fs::path& root, std::uint64_t max_mb, int max_days, const fs::path& keep) {
    std::vector<SessionRef> sessions = list_sessions(root);
    std::size_t deleted = 0;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    const auto is_kept = [&](const SessionRef& s) { return !keep.empty() && fs::equivalent(s.dir, keep, ec); };
    std::uint64_t total = 0;
    std::vector<SessionRef> remaining;
    for (const SessionRef& s : sessions) {
        const auto age = now - fs::last_write_time(s.dir / kFile, ec);
        if (!is_kept(s) && max_days > 0 && age > std::chrono::hours(24) * max_days) {
            fs::remove_all(s.dir, ec);
            ++deleted;
            continue;
        }
        total += s.bytes;
        remaining.push_back(s);
    }
    const std::uint64_t cap = max_mb << 20;
    for (auto it = remaining.rbegin(); it != remaining.rend() && max_mb > 0 && total > cap; ++it) {
        if (is_kept(*it)) continue;
        fs::remove_all(it->dir, ec);
        total -= std::min(total, it->bytes);
        ++deleted;
    }
    return deleted;
}

fs::path export_session(const fs::path& session_dir, const fs::path& dest) {
    if (!fs::exists(session_dir / kFile)) throw std::runtime_error("no journal session at " + session_dir.string());
    const fs::path target = dest / (session_dir.parent_path().filename().string() + "-" + session_dir.filename().string());
    std::error_code ec;
    fs::create_directories(target, ec);
    fs::copy(session_dir, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("cannot copy the session to " + target.string() + ": " + ec.message());
    return target;
}

std::size_t clear(const fs::path& root, std::string_view game, const fs::path& keep) {
    std::size_t deleted = 0;
    std::error_code ec;
    for (const SessionRef& s : list_sessions(root, game)) {
        if (!keep.empty() && fs::equivalent(s.dir, keep, ec)) continue;
        fs::remove_all(s.dir, ec);
        if (!ec) ++deleted;
    }
    return deleted;
}

}  // namespace anygba::journal
