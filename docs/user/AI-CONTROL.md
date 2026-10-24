# AI agents: control, debugging and the play journal

An AI agent (Claude Code, Claude Desktop, or any other MCP client) can watch a running AnyGBA game, play
it, save and load states, use cheats, read and write memory, set breakpoints and memory watches, and keep a
journal of what happened so a later session can learn from it. Everything is off until you turn it on, and
both the agent connection and the journal can be switched on and off while the game runs.

Nothing leaves your PC: the game listens on 127.0.0.1 only, and the journal is plain files in your user
folder.

## Turning it on

| | AI control | Journal |
|---|---|---|
| Menu (Escape) | Settings > AI control | Settings > Journal |
| Hotkey | F10 | F11 |
| Command line | `--control` | `--journal` |
| From the agent | `set_control` (off only) | `set_journal` |

Settings > Read-only agent (or `--control-readonly`) lets agents observe and debug but not press buttons,
change speed, load states, write memory or touch cheats. An agent can switch read-only on, never off.

When AI control turns on, the game writes a token file to `<data folder>/control/<title>-<hash>.json`
(`%APPDATA%\anyport\anygba\control\` on Windows) with the port, a random token and the process ID. Only a
program that can read that file (your user account) can connect. Turning AI control off closes every
connection at once and deletes the file. A message shows when an agent connects or leaves; while one is
connected, `AI` shows in the top-right corner of the picture, with the buttons it is pressing.

**You stay in charge.** Pressing any GBA button pauses the agent's input for three seconds
(`--takeover-frames`), and if the agent had paused the game, your key resumes it. The menu and hotkeys
keep working. When the last agent disconnects, everything it held is released: buttons, pause,
breakpoints and watches.

## Connecting an agent: anygba-mcp

`anygba-mcp` (in the package's `bin/` folder, next to `anygba`) is an MCP server on stdin/stdout. Add it to
your MCP client, for example Claude Code:

```sh
claude mcp add anygba -- "C:\anygba\bin\anygba-mcp.exe"
```

or Claude Desktop's `claude_desktop_config.json`:

```json
{ "mcpServers": { "anygba": { "command": "C:\\anygba\\bin\\anygba-mcp.exe" } } }
```

Then either start the game yourself and turn on AI control (the agent calls `list_games` and `attach`), or
let the agent start it with `launch` (`exe` = the built game; it adds `--control`; `headless` runs it with
no window and no sound, and the game's output goes to a log file in the control folder).

`anygba-mcp --control-dir DIR --journal-dir DIR` uses other folders than the per-user ones; `launch` passes
them on to the game.

### Tools

| Group | Tools |
|---|---|
| Session | `list_games`, `attach`, `launch`, `detach`, `get_info`, `set_control`, `set_journal`, `set_readonly`, `quit` |
| Observe | `screenshot` (a PNG image, `scale` 0.25-4), `get_state`, `read_memory`, `read_named`, `list_symbols`, `list_states`, `list_cheats` |
| Play | `press`, `hold`, `release`, `input_sequence`, `step_frames`, `pause`, `resume`, `set_speed` |
| Control | `save_state`, `load_state`, `set_cheat`, `write_memory` |
| Debug | `get_registers`, `disassemble`, `watch_memory`, `break_at_function`, `clear_debug`, `list_debug`, `get_call_stack`, `get_miss_log`, `tail_log` |
| Journal | `journal_search`, `journal_note`, `journal_summary`, `journal_export`, `journal_clear` |

Input is frame-accurate: `press {"buttons": "A", "frames": 6}` holds A for the next six frames and returns
when they have run, and `input_sequence` plays a list of `{buttons, frames}` steps (no buttons = wait). On a
paused game, input and `step_frames` run exactly that many frames and pause again, so an agent can play
frame by frame and look at a `screenshot` after each move. Save states taken by agents use the same
slots as the F1-F9 hotkeys (`slot`), or a `name`.

`read_named` reads per-game variables such as `money` or `party[0].hp` (see *Named memory* below).
`journal_search` and `journal_summary` also work without a running game.

### Debugging

- `break_at_function` stops when a function starts. Recompiled code can only stop at the entry of a
  recompiled ROM function; code the interpreter runs can stop at any address. Addresses inside a
  recompiled function are refused with the function's entry, and compiled RAM functions are refused.
- `watch_memory` watches 1-256 bytes of RAM, I/O, palette, VRAM or OAM for `write`, `read`, `change` or
  `equals` a value, and either stops the game (`stop`) or logs the hit (`notify`, see `tail_log`). The
  watched region's memory accesses all go through the slower bus path while a watch is set.
- A stop pauses the game where it happened, which can be in the middle of a frame; `resume` continues,
  `step_frames` runs to a frame boundary. `get_state` and `list_debug` show why the game stopped. Saving or
  loading a state from a stop first runs on to the next safe point ([../dev/SAVE-STATES.md](../dev/SAVE-STATES.md)).
- `get_registers` is exact at frame boundaries and stops; `get_call_stack` lists the return addresses of
  active compiled calls.

## The play journal

With the journal on, every session writes `<data folder>/journal/<title>-<hash>/<session>/session.jsonl`
(one JSON record per line, appended and flushed as it happens) and a `thumbs/` folder. It records:

- every agent command with its arguments, result (pictures and long memory dumps are shortened), frame
  number and client, and every error;
- what you did: save and load states, speed changes, cheat changes, AI control and read-only switches,
  taking over from the agent; `who` is `agent`, `user` or `cheat` (`cheat` lists the cheats that write
  memory every frame when the session starts);
- snapshots of the game's named symbols every 10 emulated seconds (`--journal-snapshot`, or
  `set_journal {"snapshot_seconds": N}`; only symbols marked `journal = true`);
- a half-size picture every 30 emulated seconds (`--journal-thumbnail`) and at save, load, stop and
  session start;
- the agent's notes, goals and own summaries (`journal_note`), and an end-of-session summary.

Old sessions are deleted when a new one starts: those older than 30 days (`--journal-days`, 0 keeps them)
and then the oldest until all journals fit in 512 MB (`--journal-max-mb`). `journal_export` copies a
session folder elsewhere; `journal_clear` deletes this game's (or every game's) sessions except the one
being written. You can also just delete the folders.

## Named memory (symbols)

Games can come with a symbols file that names their variables. `anygba build` copies the one for your ROM
(by game code or CRC-32) into `symbols/` next to the game, like cheat databases; `--symbols FILE` adds more.
AnyGBA ships `BPEE.toml` for Pokemon Emerald (US/EU): player name, trainer ID, play time, map and
position, money and coins (decrypted), PC and bag items, badges, steps, battles, the party (level, HP,
stats, nickname) and, during battles, both battlers with species, moves, PP and HP. The format is in
[../dev/GAME-CONFIG.md](../dev/GAME-CONFIG.md#symbols-named-memory-for-agents); a `[symbols]` table in your
own `--config` file replaces the shipped one.

## Limits

- While the menu is open the game does not answer agents; commands wait until it closes.
- Commands are answered once per frame, so an agent that sends many small commands to a running game is
  limited to about 60 a second at normal speed. `input_sequence` and `wait: false` avoid the round trips.
- The protocol is for programs on this PC only. There is no way to listen on another address.
- Protocol details for other clients: [../dev/control-protocol.md](../dev/control-protocol.md).
