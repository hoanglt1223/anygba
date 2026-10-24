#include "tools.h"

#include <algorithm>

namespace anygba::mcp {

namespace {

using control::Json;

// Shared schema pieces, as JSON text.
constexpr const char* kAddress =
    R"j({"type": ["integer", "string"], "description": "GBA address, a number or a string such as \"0x02024284\""})j";
constexpr const char* kButtons =
    R"j({"type": ["array", "string"], "items": {"type": "string"}, "description": "GBA buttons: A, B, Select, Start, Right, Left, Up, Down, R, L (a list, or a string such as \"A+Up\")"})j";

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) s.replace(pos, from.size(), to);
    return s;
}

Tool make(std::string name, std::string description, std::string properties, std::vector<std::string> required = {},
          bool local = false, bool mutating = false) {
    properties = replace_all(replace_all(properties, "$ADDRESS", kAddress), "$BUTTONS", kButtons);
    Json schema = Json::object();
    schema["type"] = "object";
    schema["properties"] = Json::parse(properties);
    if (!required.empty()) {
        Json req = Json::array();
        for (auto& r : required) req.push_back(r);
        schema["required"] = std::move(req);
    }
    return Tool{std::move(name), std::move(description), std::move(schema), local, mutating};
}

std::vector<Tool> build() {
    std::vector<Tool> t;
    // ---- session ----
    t.push_back(make("list_games", "List AnyGBA games running with AI control on (from their token files).", "{}", {}, true));
    t.push_back(make("attach",
                     "Connect to a running game. With one game running no arguments are needed; otherwise name it by "
                     "game (file stem or part of the title) or pid.",
                     R"j({"game": {"type": "string"}, "pid": {"type": "integer"}})j", {}, true));
    t.push_back(make("launch",
                     "Start a game built by `anygba build` with AI control on and attach to it. Its output goes to a log "
                     "file in the control folder.",
                     R"j({"exe": {"type": "string", "description": "path of the game executable"},
                         "args": {"type": "array", "items": {"type": "string"}, "description": "extra game options, e.g. [\"--save-dir\", \"D:/saves\"]"},
                         "headless": {"type": "boolean", "description": "no window and no sound (default false)"},
                         "journal": {"type": "boolean", "description": "start the play journal too (default false)"},
                         "readonly": {"type": "boolean", "description": "observe and debug only (default false)"},
                         "wait_seconds": {"type": "integer", "description": "how long to wait for the game (default 30)"}})j",
                     {"exe"}, true, true));
    t.push_back(make("detach", "Disconnect from the game (the game keeps running; agent input is released).", "{}", {}, true));
    t.push_back(make("get_info", "Game title, ROM hash, protocol, journal folder and the game's command list.", "{}"));
    t.push_back(make("set_control", "Turn the game's AI control server off (this disconnects). Only the user can turn it on again.",
                     R"j({"enabled": {"type": "boolean"}})j", {"enabled"}, false, true));
    t.push_back(make("set_journal", "Turn the play journal on or off and set its intervals and retention.",
                     R"j({"enabled": {"type": "boolean"},
                         "snapshot_seconds": {"type": "integer", "description": "named-symbol snapshots every N emulated seconds (0 off)"},
                         "thumbnail_seconds": {"type": "integer", "description": "thumbnails every N emulated seconds (0: only on events)"},
                         "max_mb": {"type": "integer"}, "max_days": {"type": "integer"}})j"));
    t.push_back(make("set_readonly", "Switch this game to read-only agent mode (observe and debug only). Only the user can switch it back.",
                     R"j({"enabled": {"type": "boolean", "const": true}})j", {"enabled"}));
    t.push_back(make("quit", "Close the game cleanly (saves are written, the journal session ends).", "{}", {}, false, true));
    // ---- observe ----
    t.push_back(make("screenshot", "The current picture as a PNG image (240x160 times scale).",
                     R"j({"scale": {"type": "number", "description": "0.25 to 4, default 1"}})j"));
    t.push_back(make("get_state",
                     "Frame number, fps, speed, paused and why, buttons the agent holds, user override, gamepad, active "
                     "cheats, journal, frame hash.",
                     "{}"));
    t.push_back(make("read_memory", "Read guest memory (hex; `value` too for 1, 2 or 4 bytes, little-endian). No side effects.",
                     R"j({"address": $ADDRESS, "length": {"type": "integer", "description": "1 to 65536, default 16"}})j", {"address"}));
    t.push_back(make("read_named",
                     "Read per-game named memory, e.g. \"money\", \"party[0].hp\" or \"party\" (list_symbols shows the names).",
                     R"j({"name": {"type": "string"}, "names": {"type": "array", "items": {"type": "string"}}})j"));
    t.push_back(make("list_symbols", "The game's named memory: names, types, addresses, struct fields.", "{}"));
    t.push_back(make("list_states", "Save states on disk: slots 1-9 (shared with the player's hotkeys) and named states.", "{}"));
    t.push_back(make("list_cheats", "Cheats from the game's databases and the user's codes, with what is on.",
                     R"j({"filter": {"type": "string"}, "limit": {"type": "integer"}})j"));
    // ---- play ----
    t.push_back(make("press",
                     "Press buttons for N frames from the next frame, then return (the game runs those frames even when "
                     "paused). The user pressing anything pauses agent input for a while (user_override in the result).",
                     R"j({"buttons": $BUTTONS, "frames": {"type": "integer", "description": "default 6"},
                         "release_frames": {"type": "integer", "description": "frames with the buttons up afterwards (default 0)"},
                         "wait": {"type": "boolean", "description": "return at once instead of after the frames (default true)"}})j",
                     {"buttons"}, false, true));
    t.push_back(make("hold", "Hold buttons until release.", R"j({"buttons": $BUTTONS})j", {"buttons"}, false, true));
    t.push_back(make("release", "Release held buttons (all when none are given).", R"j({"buttons": $BUTTONS})j", {}, false, true));
    t.push_back(make("input_sequence",
                     "A frame-accurate input script: each step holds its buttons (none = wait) for its frames.",
                     R"j({"steps": {"type": "array", "items": {"type": "object", "properties": {"buttons": $BUTTONS, "frames": {"type": "integer"}}}},
                         "wait": {"type": "boolean"}})j",
                     {"steps"}, false, true));
    t.push_back(make("step_frames", "Run N frames (also while paused), then return.", R"j({"frames": {"type": "integer"}})j", {},
                     false, true));
    t.push_back(make("pause", "Pause the game at the next frame boundary.", "{}", {}, false, true));
    t.push_back(make("resume", "Resume a paused or stopped game.", "{}", {}, false, true));
    t.push_back(make("set_speed", "Emulation speed: 100, 200, 300 or 400 percent.", R"j({"percent": {"type": "integer"}})j",
                     {"percent"}, false, true));
    // ---- control ----
    t.push_back(make("save_state", "Save a state to a slot (1-9) or a name. Journaled.",
                     R"j({"slot": {"type": "integer"}, "name": {"type": "string"}})j", {}, false, true));
    t.push_back(make("load_state", "Load a state from a slot (1-9) or a name. Journaled.",
                     R"j({"slot": {"type": "integer"}, "name": {"type": "string"}})j", {}, false, true));
    t.push_back(make("set_cheat",
                     "Turn a cheat on or off or set its value (by index or name), or add a code (code + format). Journaled.",
                     R"j({"index": {"type": "integer"}, "name": {"type": "string"}, "enabled": {"type": "boolean"},
                         "value": {"type": ["integer", "string"]},
                         "code": {"type": ["array", "string"], "items": {"type": "string"}},
                         "format": {"type": "string", "description": "auto, raw, gsv1, gsv1raw, arv3, arv3raw, cb"},
                         "persistent": {"type": "boolean"}})j",
                     {}, false, true));
    t.push_back(make("write_memory", "Write RAM, I/O, palette, VRAM or OAM: hex bytes, or value + size (1, 2, 4). Journaled.",
                     R"j({"address": $ADDRESS, "hex": {"type": "string"}, "value": {"type": ["integer", "string"]}, "size": {"type": "integer"}})j",
                     {"address"}, false, true));
    // ---- debug ----
    t.push_back(make("get_registers", "CPU registers, mode, flags.", "{}"));
    t.push_back(make("disassemble", "Disassemble from an address (default: the PC).",
                     R"j({"address": $ADDRESS, "count": {"type": "integer"}, "mode": {"type": "string", "enum": ["auto", "arm", "thumb"]}})j"));
    t.push_back(make("watch_memory",
                     "Watch memory: stop (pause) or notify (tail_log) on write, read, change, or when it becomes a value.",
                     R"j({"address": $ADDRESS, "length": {"type": "integer"}, "on": {"type": "string", "enum": ["write", "read", "change", "equals"]},
                         "value": {"type": ["integer", "string"]}, "action": {"type": "string", "enum": ["stop", "notify"]}})j",
                     {"address"}));
    t.push_back(make("break_at_function",
                     "Breakpoint at the entry of a compiled ROM function, or at any address of code the interpreter runs.",
                     R"j({"address": $ADDRESS, "action": {"type": "string", "enum": ["stop", "notify"]}})j", {"address"}));
    t.push_back(make("clear_debug", "Remove a breakpoint or watch by id (all when no id).", R"j({"id": {"type": ["integer", "string"]}})j"));
    t.push_back(make("list_debug", "Breakpoints and watches with hit counts, and why the game is stopped.", "{}"));
    t.push_back(make("get_call_stack", "Return addresses of active compiled call sites, plus pc, lr, sp.", "{}"));
    t.push_back(make("get_miss_log", "Addresses the interpreter had to run (the recompiler missed them).",
                     R"j({"limit": {"type": "integer"}})j"));
    t.push_back(make("tail_log", "The game's recent log: agent commands' effects, watch and breakpoint hits, errors, connections.",
                     R"j({"lines": {"type": "integer"}})j"));
    // ---- journal ----
    t.push_back(make("journal_search",
                     "Search play journals (this game by default; works without a running game) by text, kind, who, method.",
                     R"j({"text": {"type": "string"}, "kind": {"type": "string"}, "who": {"type": "string", "enum": ["agent", "user", "cheat"]},
                         "method": {"type": "string"}, "session": {"type": "string"}, "game": {"type": "string", "description": "\"all\", or a game name when not attached"},
                         "limit": {"type": "integer"}})j"));
    t.push_back(make("journal_note", "Write a note, a goal, or your own session summary into the journal.",
                     R"j({"text": {"type": "string"}, "kind": {"type": "string", "enum": ["note", "goal", "summary"]}})j", {"text"}));
    t.push_back(make("journal_summary",
                     "Summary of a journal session (default: the current or latest one): counts, commands, notes, goals, errors, last snapshot.",
                     R"j({"session": {"type": "string"}, "game": {"type": "string"}})j"));
    t.push_back(make("journal_export", "Copy a journal session folder to another folder.",
                     R"j({"to": {"type": "string"}, "session": {"type": "string"}})j", {"to"}));
    t.push_back(make("journal_clear", "Delete this game's (or all) journal sessions except the active one.",
                     R"j({"scope": {"type": "string", "enum": ["game", "all"]}})j", {}, false, true));
    return t;
}

}  // namespace

const std::vector<Tool>& tools() {
    static const std::vector<Tool> all = build();
    return all;
}

const Tool* find_tool(const std::string& name) {
    const auto& all = tools();
    const auto it = std::find_if(all.begin(), all.end(), [&](const Tool& t) { return t.name == name; });
    return it == all.end() ? nullptr : &*it;
}

}  // namespace anygba::mcp
