#pragma once

#include "control/json.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The play journal (docs/user/AI-CONTROL.md "Journal"): an append-only JSONL audit trail per
// session under <root>/<game>/<session>/session.jsonl, with frame thumbnails beside it. It
// records commands and their results, who issued them (agent, user, cheat), state saves and
// loads, memory writes, cheat and speed changes, errors, agent notes and goals, periodic
// snapshots of named symbols, and an end-of-session summary. Nothing leaves the PC.
namespace anygba::journal {

using control::Json;

struct Settings {
    int snapshot_seconds = 10;     // named-symbol snapshots, in emulated seconds; 0 = off
    int thumbnail_seconds = 30;    // pictures, in emulated seconds; 0 = only on events
    std::uint64_t max_mb = 512;    // retention: total size of all sessions
    int max_days = 30;             // retention: sessions older than this are deleted
};

class Journal {
public:
    Journal(std::filesystem::path root, std::string game);
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    // Opens a new session (after applying retention) and records `session_start` with `info`.
    // Throws std::runtime_error when the folder cannot be written.
    void start(std::uint64_t frame, Json info);
    // Records `session_end` with the summary and closes the session.
    void stop(std::uint64_t frame);
    bool active() const { return out_.is_open(); }

    Settings& settings() { return settings_; }
    const std::filesystem::path& root() const { return root_; }
    const std::string& game() const { return game_; }
    const std::string& session_id() const { return session_; }
    std::filesystem::path session_dir() const { return root_ / game_ / session_; }

    // Appends {seq, time, frame, kind, who, ...data}. A no-op while inactive.
    void record(std::uint64_t frame, std::string_view kind, std::string_view who, Json data = Json::object());
    // Writes a half-size PNG of `picture` (240x160 RGB555) and records it.
    void thumbnail(std::uint64_t frame, std::span<const std::uint16_t> picture, std::string_view reason);
    // Once per emulated frame: periodic symbol snapshots and thumbnails.
    void on_frame(std::uint64_t frame, const std::function<Json()>& snapshot, std::span<const std::uint16_t> picture);

private:
    std::filesystem::path root_;
    std::string game_;
    std::string session_;
    std::ofstream out_;
    Settings settings_;
    std::uint64_t seq_ = 0;
    std::uint64_t start_frame_ = 0;
    std::uint64_t next_snapshot_ = 0;
    std::uint64_t next_thumbnail_ = 0;
};

// ---- reading journals (anygba-mcp, tests): plain files, no running game needed ----

struct SessionRef {
    std::string game;
    std::string id;
    std::filesystem::path dir;
    std::uint64_t bytes = 0;
};
// Newest first. `game` empty: every game.
std::vector<SessionRef> list_sessions(const std::filesystem::path& root, std::string_view game = {});

struct Query {
    std::string text;    // case-insensitive substring of the record
    std::string kind;    // exact
    std::string who;     // exact
    std::string method;  // exact, for command records
    std::string game;
    std::string session;
    std::size_t limit = 50;
};
// Matching records, newest first, each with "game" and "session" added.
Json search(const std::filesystem::path& root, const Query& query);
// Counts, commands, notes, goals, errors and the last snapshot of one session, computed from
// its file (so it also works for a session that ended in a crash).
Json summarize(const std::filesystem::path& session_dir);
// Deletes sessions older than `max_days`, then the oldest until the total is under `max_mb`.
// `keep` (the active session) is never deleted. Returns the number deleted.
std::size_t apply_retention(const std::filesystem::path& root, std::uint64_t max_mb, int max_days,
                            const std::filesystem::path& keep = {});
// Copies a session folder into `dest` (created). Returns the copy's path.
std::filesystem::path export_session(const std::filesystem::path& session_dir, const std::filesystem::path& dest);
// Deletes the sessions of `game` (every game when empty) except `keep`. Returns the count.
std::size_t clear(const std::filesystem::path& root, std::string_view game, const std::filesystem::path& keep = {});

}  // namespace anygba::journal
