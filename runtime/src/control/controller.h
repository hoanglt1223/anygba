#pragma once

#include "anygba/system.hpp"
#include "cheats/cheat_list.h"
#include "control/json.h"
#include "control/server.h"
#include "control/symbols.h"
#include "journal/journal.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Agent control of a running game (docs/dev/control-protocol.md): the command handlers on top
// of the transport in server.h, a frame-accurate input queue, pause and step, save states at
// safe points, cheats, memory, breakpoints and watches, and the play journal. Everything runs
// on the game's thread at frame boundaries, and both the server and the journal can be turned
// on and off at any time.
namespace anygba::control {

// What the controller needs from the player. Every hook is called on the game's thread.
struct Hooks {
    std::function<int()> speed;
    std::function<void(int)> set_speed;
    std::function<std::optional<std::filesystem::path>()> state_dir;
    std::function<std::string()> state_stem;  // file stem of save states ("<title>-<hash8>")
    std::function<void()> after_load;         // resets audio after a state load
    std::function<void(const std::string&)> osd;
    std::function<void()> cheats_changed;     // syncs ROM patches and saves the cheat file
    // While paused: shows the picture and pumps window events. Returns false when the window
    // closed. `user_keys` receives the GBA buttons the user holds.
    std::function<bool(std::uint16_t& user_keys)> idle;
    std::function<double()> fps;
    std::function<std::string()> pad_name;    // connected gamepad, "" for none
    std::function<void()> quit;               // ends the game after this frame
};

struct Settings {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;  // 0: any free port
    std::filesystem::path token_file;
    bool readonly = false;     // observe and debug tools only
    int takeover_frames = 180; // a user keypress pauses agent input for this long
    Json info;                 // title, game, rom_sha256, exe: in the token file and `hello`
};

class Controller {
public:
    Controller(System& sys, cheats::CheatList& cheats, Hooks hooks, Settings settings, Symbols symbols,
               std::unique_ptr<journal::Journal> journal);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    // ---- runtime toggles (menu, hotkeys, command line, agents) ----
    // Starts the server. Returns an error message instead of throwing.
    std::optional<std::string> set_control(bool on, std::string_view who);
    bool control_on() const { return server_.running(); }
    std::optional<std::string> set_journal(bool on, std::string_view who);
    bool journal_on() const { return journal_ && journal_->active(); }
    void set_readonly(bool on, std::string_view who);
    bool readonly() const { return settings_.readonly; }
    journal::Journal* journal() { return journal_.get(); }
    const Symbols& symbols() const { return symbols_; }

    // ---- once per emulated frame, from VBlank ----
    // The buttons for this frame: the user's, plus the agent's unless the user has pressed
    // something recently (takeover).
    std::uint16_t keys(std::uint16_t user_keys);
    // Answers requests, finishes timed commands, journals, and blocks while paused.
    void service_frame();

    // Records something the user (or a cheat) did, for the journal and tail_log.
    void user_event(std::string_view kind, Json data, std::string_view who = "user");
    void log(std::string line);
    // OSD indicator: "" when no agent is connected.
    std::string indicator() const;
    std::size_t clients() const { return server_.clients(); }

private:
    enum class Category : std::uint8_t { Session, Observe, Play, Control, Debug, Journal };
    enum class Where : std::uint8_t { Frame, SafePoint, Stop };
    struct Pending {
        Request request;
        std::uint64_t from = 0;   // first frame of its input
        std::uint64_t until = 0;  // answered at this emulated frame
        Json result;
    };
    struct Timed {
        std::uint16_t mask = 0;
        std::uint64_t from = 0, until = 0;  // frames [from, until)
    };
    struct Breakpoint {
        int id = 0;
        std::uint32_t addr = 0;
        bool stop = true;
        std::uint64_t hits = 0;
    };
    struct Watch {
        int id = 0;
        std::uint32_t addr = 0;
        std::uint32_t size = 1;
        std::string kind;  // write, read, change, equals
        std::uint32_t value = 0;
        bool stop = true;
        std::uint64_t hits = 0;
        std::vector<std::uint8_t> shadow;
    };
    using Handler = std::function<void(Controller&, const Request&)>;
    struct Method {
        Category category;
        Handler handler;
    };

    static const std::map<std::string, Method>& methods();
    void handle(const Request& r);
    void finish(const Request& r, Json result);
    void fail(const Request& r, int code, const std::string& message);
    void journal_command(const Request& r, const Json* result, const std::string* error);
    void poll_and_handle(int timeout_ms);
    void complete_due();
    void wait_while_paused(Where where);
    void stop_here(Json reason);
    void at_safe_point(const Request& r, std::function<Json(System&)> op);
    void drop_agent_state();
    void sync_watches();
    void sync_breakpoints();
    void on_access(const System::Access& a);
    Peek peeker();
    std::uint16_t agent_mask(std::uint64_t frame) const;
    Json info() const;
    Json state_json();
    std::filesystem::path state_file(const Json& params) const;
    void schedule_input(const Request& r, const std::vector<std::pair<std::uint16_t, std::uint32_t>>& steps);

    // handlers
    void h_hello(const Request& r);
    void h_get_state(const Request& r);
    void h_screenshot(const Request& r);
    void h_read_memory(const Request& r);
    void h_read_named(const Request& r);
    void h_list_symbols(const Request& r);
    void h_press(const Request& r);
    void h_hold(const Request& r);
    void h_release(const Request& r);
    void h_input_sequence(const Request& r);
    void h_step_frames(const Request& r);
    void h_pause(const Request& r);
    void h_resume(const Request& r);
    void h_set_speed(const Request& r);
    void h_save_state(const Request& r);
    void h_load_state(const Request& r);
    void h_list_states(const Request& r);
    void h_list_cheats(const Request& r);
    void h_set_cheat(const Request& r);
    void h_write_memory(const Request& r);
    void h_get_registers(const Request& r);
    void h_disassemble(const Request& r);
    void h_watch_memory(const Request& r);
    void h_break_at_function(const Request& r);
    void h_clear_debug(const Request& r);
    void h_list_debug(const Request& r);
    void h_get_call_stack(const Request& r);
    void h_get_miss_log(const Request& r);
    void h_tail_log(const Request& r);
    void h_set_control(const Request& r);
    void h_set_journal(const Request& r);
    void h_set_readonly(const Request& r);
    void h_journal_note(const Request& r);
    void h_journal_summary(const Request& r);
    void h_journal_search(const Request& r);
    void h_journal_export(const Request& r);
    void h_journal_clear(const Request& r);
    void h_quit(const Request& r);

    System& sys_;
    cheats::CheatList& cheats_;
    Hooks hooks_;
    Settings settings_;
    Symbols symbols_;
    std::unique_ptr<journal::Journal> journal_;
    Server server_;

    bool paused_ = false;
    std::uint64_t run_until_ = 0;   // while paused, run until this frame
    Where where_ = Where::Frame;
    bool servicing_ = false;        // handling a request: debugger hooks stay quiet
    Json stop_reason_;
    std::vector<Pending> pending_;
    std::uint16_t hold_ = 0;
    std::vector<Timed> timed_;
    std::uint64_t takeover_until_ = 0;
    std::uint16_t last_agent_mask_ = 0;
    std::map<int, Breakpoint> breakpoints_;
    std::map<int, Watch> watches_;
    int next_debug_id_ = 1;
    std::deque<std::string> log_;
};

// "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L" (case-insensitive) to a
// KEYINPUT mask. Throws std::runtime_error for unknown names.
std::uint16_t parse_buttons(const Json& buttons);
std::string button_names(std::uint16_t mask);

}  // namespace anygba::control
