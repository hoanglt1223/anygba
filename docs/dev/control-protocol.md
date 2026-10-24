# Agent control protocol

The protocol between a running AnyGBA game and its clients (`anygba-mcp`, tests, or your own program).
User guide: [../user/AI-CONTROL.md](../user/AI-CONTROL.md). Code: `runtime/src/control/` (transport in
`server.*`, commands in `controller.*`), `runtime/src/journal/`, `tools/mcp/`. Tests:
`tests/runtime/control_tests.cpp`, `tests/mcp/mcp-e2e.py` (CTest `mcp-e2e`).

## Transport

- TCP on 127.0.0.1 only. `--control-host` accepts `127.0.0.1` and `localhost` and nothing else; the game
  exits with status 2 for any other value. Accepted connections from a non-loopback peer are closed.
- Newline-delimited JSON-RPC 2.0: one request object per line, one response per line. Lines over 8 MB
  close the connection. Batches and notifications are not used.
- Each connection gets one request at a time: the next request is read after the previous one has been
  answered. Commands that span frames (`press`, `step_frames`, ...) answer when they finish, so a client
  that wants to do something else meanwhile opens a second connection.
- Requests are handled on the game's thread at frame boundaries (VBlank), at a stop, or at a save-state
  safe point. Nothing runs concurrently with the emulated machine.

### Token file

Written when the server starts (atomically, mode 0600 on POSIX) and deleted when it stops:
`<control dir>/<title>-<sha8>.json`, where the control dir is `--control-dir` or `<data folder>/control`
(`SDL_GetPrefPath("anyport", "anygba")`, `%APPDATA%\anyport\anygba\` on Windows).

```json
{"protocol":1,"port":51234,"token":"<48 hex digits>","pid":4321,"title":"POKEMON EMER",
 "game":"POKEMON_EMER-a9dec84d","rom_sha256":"...","exe":"C:\\games\\POKEMON EMER.exe","readonly":false}
```

A file whose `pid` is not running is stale (the game crashed); `anygba-mcp list_games` deletes it.

### Handshake

The first request must be `hello` with the token. Any other first request, or a wrong token, gets error
`-32001` and the connection is closed.

```json
{"jsonrpc":"2.0","id":1,"method":"hello","params":{"token":"...","client":"my-agent"}}
{"jsonrpc":"2.0","id":1,"result":{"title":"...","game":"...","rom_sha256":"...","protocol":1,"pid":4321,
 "readonly":false,"journal":true,"journal_root":"...","symbols":27,"methods":["break_at_function",...],"frame":812}}
```

### Errors

| Code | Meaning |
|---|---|
| -32700 | not JSON |
| -32600 | not a request (no `method`, line too long) |
| -32601 | unknown method |
| -32602 | bad parameters (the message names the parameter) |
| -32001 | no `hello` yet, or wrong token |
| -32002 | read-only agent mode |
| -32003 | the command failed (unreadable memory, no such state, ...) |

## Frames, pause and input

Let F be the frame number when a command is handled (`frame` in most results; it counts VBlanks since
reset and goes back when a state is loaded).

- Input commands schedule buttons for frames F+1 onwards and answer at frame F+n, after the game has seen
  them for n frames. Agent buttons are ORed with the user's. A user keypress suppresses agent buttons for
  `--takeover-frames` (default 180); results then carry `"user_override": true`.
- `pause` pauses at the frame boundary where it is handled. While paused, input commands and `step_frames` run their frames and
  pause again (`run until F+n`).
- A breakpoint or watch with `"action": "stop"` pauses mid-frame; pending frame commands are answered at
  once with `"interrupted": {reason}`. `get_state.stop_reason` says why the game is paused.
- `save_state` and `load_state` run at the next safe point ([SAVE-STATES.md](SAVE-STATES.md)): the game
  runs on from a pause or stop until it reaches one, does the operation, answers, and stays paused there if
  it was paused. A load answers pending frame commands with `"interrupted": {"reason": "load_state"}` and
  drops queued input. Saving and then loading give the same frames afterwards (the e2e test checks the
  frame hash 60 frames later).
- When the last client disconnects, held and queued buttons, the pause, breakpoints and watches are
  dropped. Turning control off (menu, F10, `set_control`) does the same and closes every connection.

## Methods

Addresses are numbers or strings (`"0x02024284"`). Buttons are `A B Select Start Right Left Up Down R L`
(any case), as a list or a string such as `"A+Up"`.

| Method | Params | Result |
|---|---|---|
| `hello`, `get_info` | `token`, `client` (hello) | game info, see above |
| `get_state` | | `frame`, `fps`, `speed`, `paused`, `stop_reason`, `agent_buttons`, `user_override_frames`, `pad`, `active_cheats`, `readonly`, `journal`, `journal_session`, `clients`, `frame_hash` |
| `screenshot` | `scale` (0.25-4) | `png_base64`, `width`, `height`, `frame`, `frame_hash` |
| `read_memory` | `address`, `length` (1-65536) | `hex`, `region`, `value` (1, 2 or 4 bytes, little-endian) |
| `read_named` | `name` or `names` | `value` or `values` |
| `list_symbols` | | `symbols`: name, type, address, layout |
| `list_states` | | `states`: slot or name, bytes; `dir` |
| `list_cheats` | `filter`, `limit` | `cheats`: index, name, enabled, value, choices...; `matching` |
| `press` | `buttons`, `frames` (1-36000, default 6), `release_frames`, `wait` | `from_frame`, `frames`, `frame` |
| `hold` / `release` | `buttons` (release: default all) | `held` |
| `input_sequence` | `steps`: [{`buttons`, `frames`}] (up to 1000 steps, 36000 frames), `wait` | as `press` |
| `step_frames` | `frames` | `frame` |
| `pause` / `resume` | | `paused`, `frame` |
| `set_speed` | `percent`: 100, 200, 300, 400 | `speed` |
| `save_state` | `slot` (1-9) or `name` | `file`, `bytes`, `frame` |
| `load_state` | `slot` or `name` | `file`, `frame`, `frame_hash` |
| `set_cheat` | `index` or `name`, `enabled`, `value`; or `code`, `format`, `name`, `persistent` to add | `index`, `name`, `enabled`, `value` |
| `write_memory` | `address`, `hex` (up to 4096 bytes) or `value` + `size` (1, 2, 4) | `before`, `after`, `bytes` |
| `get_registers` | | `r0`-`r15`, `cpsr`, `spsr`, `mode`, `thumb`, `flags` |
| `disassemble` | `address` (default PC), `count` (1-200), `mode` (auto, arm, thumb) | `lines`: address, bytes, text, `compiled_function`, `pc`, `breakpoint` |
| `watch_memory` | `address`, `length` (1-256), `on` (write, read, change, equals), `value`, `action` (stop, notify) | `id` |
| `break_at_function` | `address`, `action` | `id`, `kind` (compiled_entry, interpreted) |
| `clear_debug` | `id` (default all) | `removed` |
| `list_debug` | | `breakpoints`, `watches` (with `hits`), `stop_reason` |
| `get_call_stack` | | `pc`, `lr`, `sp`, `return_addresses` |
| `get_miss_log` | `limit` | `count`, `addresses`, `file` |
| `tail_log` | `lines` | `lines` |
| `set_control` | `enabled: false` | answered, then the server stops |
| `set_journal` | `enabled`, `snapshot_seconds`, `thumbnail_seconds`, `max_mb`, `max_days` | `enabled`, `session`, `dir` |
| `set_readonly` | `enabled: true` | `readonly` |
| `journal_note` | `text` (up to 4000 chars), `kind` (note, goal, summary) | `recorded` |
| `journal_summary` | `session` (default: current or latest) | summary, see below |
| `journal_search` | `text`, `kind`, `who`, `method`, `session`, `game` ("all"), `limit` | `records` |
| `journal_export` | `to` (absolute folder), `session` | `exported_to` |
| `journal_clear` | `scope` (game, all) | `deleted_sessions` |
| `quit` | | answered, then the game ends after this frame |

Read-only mode refuses play and control methods (`press` ... `set_speed`, `save_state`, `load_state`,
`set_cheat`, `write_memory`, `journal_clear`, `quit`) and stopping breakpoints and watches.

Memory reads (`read_memory`, `read_named`, `disassemble`) come straight from the backing arrays: no side
effects, no watch hits, and I/O registers read as last written. Writes go through the bus, so I/O writes
act as the game's would; BIOS and ROM are not writable (ROM changes are cheats).

## Journal records

`session.jsonl` holds one object per line: `seq`, `time` (UTC, ISO 8601), `frame`, `kind`, `who`
(`agent`, `user`, `cheat`) and fields by kind:

| Kind | Fields |
|---|---|
| `session_start` | title, game, rom_sha256, exe, `by`, `settings` |
| `command` | `method`, `client`, `params`, `result` or `error` |
| `snapshot` | `symbols`: values of `journal = true` symbols |
| `thumbnail` | `file` (relative), `reason` (interval, save_state, load_state, stop, session_start) |
| `note`, `goal`, `summary` | `text`, `client` |
| `stop` | `reason` |
| `state_save`, `state_load` | `slot` (the user's hotkeys and menu) |
| `speed`, `cheats`, `control`, `readonly`, `journal`, `takeover` | what changed |
| `agent_connected`, `agent_disconnected` | `client` |
| `cheats_active` | `cheats` |
| `session_end` | `summary`, `frames_played` |

The summary (`journal_summary`, `session_end`) is computed from the file, so it also works for a session
that ended in a crash: counts by kind, who and command, state saves and loads, memory writes, cheat
changes, the last notes, goals, agent summaries and errors, the last snapshot, and `ended_cleanly`.
