# Using AnyGBA

AnyGBA turns a GBA ROM you own into a native program. You need the AnyGBA package (a release archive,
or `cmake --install` of a build), CMake 3.28+, and a C/C++ compiler (Visual Studio 2022 on Windows,
GCC or Clang on Linux, Xcode command-line tools on macOS). No ROM, BIOS or game data ships with AnyGBA.

## One command

```sh
<prefix>/bin/anygba build game.gba [--config game.toml] [-o dist]
```

This recompiles the ROM to C, compiles it against the runtime in `<prefix>`, and writes:

| Host | Output |
|---|---|
| Windows | `dist/<Title>.exe` |
| Linux | `dist/<Title>` |
| macOS | `dist/<Title>.app` (ad-hoc signed; the ROM is in `Contents/Resources/game.gba`) |

On Windows and Linux the ROM is appended to the executable, so the program runs on its own. The program
checks the ROM's SHA-256 and refuses any other ROM. Share the built program only with people who own the
game: it contains the ROM.

Options (`anygba help build`):

| Option | Meaning |
|---|---|
| `--config FILE` | per-game config: entry points, RAM code, save type, patches ([GAME-CONFIG.md](../dev/GAME-CONFIG.md)) |
| `--miss-log FILE` | trace-feedback log from an earlier run (repeatable) |
| `-o, --output DIR` | output folder (default: current folder) |
| `--work DIR` | generated C and build files (default: `<output>/<rom name>-build`) |
| `--name NAME` | program name (default: the game title from the ROM header or config) |
| `--prefix DIR` | AnyGBA package (default: the folder above `anygba`'s `bin/`) |
| `--cmake PATH`, `--generator NAME`, `--build-type TYPE` | how CMake is run (defaults: `cmake`, CMake's choice, `Release`) |
| `--no-embed` | leave the ROM out; the program then takes it as its first argument |

## Running a game

```sh
<Title> [rom.gba] [--scale N] [--fast] [--save-dir DIR] [--input FILE] ...
```

`<Title> --help` lists every option. Useful ones:

- `--scale N`: window size as a multiple of 240x160 (default 3).
- `--speed N`: start at 100, 200, 300 or 400% speed.
- `--ff-speed N|unlimited`: fast-forward speed for this run (see Speed).
- `--load-state FILE`: start from a save state.
- `--cheat CODE`, `--cheats FILE`, `--cheat-db FILE`: cheats ([CHEATS.md](CHEATS.md)).
- `--filter SPEC`, `--video SETTINGS`: graphics filters, colour correction and output resolution, also in
  the menu's Video page ([VIDEO.md](VIDEO.md)).
- `--headless --frames N --dump-frame-hash`: no window or audio; prints the SHA-256 of the last frame.
  Used by tests and the compatibility tracker.
- `--exit-on-idle`: stop at the first branch-to-self (test ROMs end this way) and print the registers.
- `--dump REGION:FILE`: write memory (`ewram`, `iwram`, `io`, `palette`, `vram`, `oam`) or the last
  picture (`frame`, 240x160 RGB555) to a file on exit. Handy for finding cheat addresses.
- `--wav FILE`: record the sound as it is played (after speed changes) to a WAV file, also headless.
- `--control`, `--journal`, `--symbols FILE` and the other `--control-*` and `--journal-*` options: AI agent
  control and the play journal ([AI-CONTROL.md](AI-CONTROL.md)).

## Hotkeys

| Key (default) | Gamepad (default) | Action |
|---|---|---|
| F1 to F9 | Back+LeftShoulder (slot 1) | load save state 1-9 |
| Shift+F1 to Shift+F9 | Back+RightShoulder (slot 1) | save state 1-9 |
| Space (hold) | RightTrigger (hold) | fast-forward at the fast-forward speed (default 300%) |
| `=` / `-` | Back+DpadUp / Back+DpadDown | speed up / down: 100, 200, 300, 400% |
| Escape | Back+Start | open or close the menu |
| F12 (hold) | | the cheat device's button, for codes that need it |
| F10 | | AI control on/off ([AI-CONTROL.md](AI-CONTROL.md)) |
| F11 | | play journal on/off |
| Ctrl+F | | stats overlay on/off (see below) |

All of them can be changed in the Controls page of the menu or in
[INPUT_MAPPING.md](INPUT_MAPPING.md). Messages about slots, speed, cheats and gamepads appear at the bottom
left of the picture for a few seconds.

## Speed

Speed steps through 100, 200, 300 and 400%. The sound plays faster and higher with the game instead of
breaking up: each frame's sound is resampled by the speed factor, so the sound card still sets the pace.
The starting speed is `--speed`, else `Speed` in the input file's `[player]` section.

Holding fast-forward runs at the fast-forward speed: 150, 200, 300 (default), 400, 500 or 800%, with the
sound resampled the same way, or *unlimited*, which runs as fast as the computer can and drops the sound
the card cannot keep up with. Set it in the menu (Speed page), with `FastForwardSpeed` in the input file's
`[player]` section, or for one run with `--ff-speed N|unlimited`. A change in the menu is saved to the
input file.

*Normal-speed music* (Speed page, `NormalSpeedMusic = on` in `[player]`; off by default) keeps the music
at 1x while the game runs faster, at any speed above 100% and while fast-forward is held (not at
*unlimited*). In games that use Nintendo's MusicPlayer2000 sound engine (most first-party and many other
games, Pokemon Emerald among them), the engine's sequencer is slowed by the speed, so songs and sound
effects keep their tempo; the engine's note envelopes are slowed too, while its GB-channel envelopes still
run at game speed. In every game the sound is then time-stretched instead of resampled, so it keeps its
pitch; in games without that engine the music plays faster at its normal pitch.
`tests/normal-speed-music.py --game <your game>` checks the tempo on a build of your own Pokemon
Emerald dump.

## Stats overlay

Ctrl+F (the `Stats` hotkey) shows figures in the top-left corner, updated every second: pictures shown
and GBA frames emulated per second, the measured speed against its target (`(FF)` while fast-forward is
held), the host time to emulate one frame, how full the sound queue is and how many times the game's
sound DMA fell behind (FIFO underruns), the share of emulated time spent in the interpreter (code the
recompiler missed; see "When something is missing"), the video filter and output resolution, the
number of active cheats, the save type and whether the cartridge clock is on. It is off by default; the
choice is saved as `Stats = on|off` in the input file's `[player]` section. Headless runs print the
lines as `stats| ...`.

## Save states

Nine slots per game, like VisualBoyAdvance: Shift+F1 saves to slot 1, F1 loads it. State files are next to
the save file, as `<title>-<hash>.ss1` to `.ss9`. A state includes the save chip, so loading one also brings
back the in-game save of that moment. States record the ROM's SHA-256 and the AnyGBA state format version;
a state from another game, a damaged file, or one from an incompatible AnyGBA version is refused with a
message and the game goes on. How states work with recompiled code: [../dev/SAVE-STATES.md](../dev/SAVE-STATES.md).

## Menu

Escape (gamepad: Back+Start) pauses the game and opens the menu; the GBA buttons move through it (d-pad,
A to choose, B to go back, L/R to page). Escape or the same gamepad chord closes it.

- **Save state / Load state**: Left/Right picks the slot.
- **Speed**: Left/Right changes the speed; A opens the Speed page with *Speed*, *Fast-forward speed* and
  *Normal-speed music*.
- **Cheats**: the cheat databases for this game and your own codes ([CHEATS.md](CHEATS.md)).
- **Controls**: every GBA button and hotkey with its keyboard and gamepad bindings. Choose one, then press
  the new key (with Ctrl, Alt or Shift for a hotkey chord) or gamepad button (hold another gamepad button
  first for a chord such as Back+Start). If that key was bound to something else, the two swap. Escape
  cancels. *Reset to defaults* restores the defaults. Changes are saved to the input mapping file at once.
- **Video**: filter, screen effect, ghosting, colour, gamma, debanding, resolution, fullscreen and integer
  scale; Left/Right changes them at once and they are kept for this game ([VIDEO.md](VIDEO.md)).
- **Settings**: AI control, Read-only agent and Journal switches ([AI-CONTROL.md](AI-CONTROL.md)). They
  take effect at once and last until the game ends.

## Gamepads

Gamepads can be plugged in and out while playing; a message names the controller. Any controller SDL
knows is mapped by button position (Xbox, PlayStation, Switch and most others). For an unknown controller,
put a `gamecontrollerdb.txt` (the [SDL GameController database](https://github.com/mdqinc/SDL_GameControllerDB)
format) next to the program, or set the `SDL_GAMECONTROLLERCONFIG` environment variable. The left stick also
moves the d-pad (`StickDeadzone` in [INPUT_MAPPING.md](INPUT_MAPPING.md)).

## Scripted input

`--script FILE` plays input by host frame (every VBlank, plus every frame the menu is open), for tests and
automation; headless runs print the menu text and messages so a script's effect can be checked:

```text
# <frame> key|pad|gba down|up|tap <name>    key names are SDL names; chords like Shift+F1 or Back+Start
120 key tap Shift+F1
200 gba down A
230 gba up A
240 pad tap Start
250 stick 0 -30000            # left stick x y
260 type 82025E300063         # one key tap every two frames
270 connect Test Pad          # as if a gamepad was plugged in
```

`--keys a:10-20,start:30-31` holds GBA buttons over frame ranges (the syntax of `anygba-shot`).

## Saves

The backup memory type is read from the ROM's library ID string (`SRAM_V`, `FLASH_V`, `FLASH512_V`,
`FLASH1M_V`, `EEPROM_V`). A per-game config (`[game] save = "..."`) or `--save-type` overrides it.
EEPROM size (512 bytes or 8 KB) is taken from the game's first EEPROM transfer, or from an existing save
file.

Games that had a real-time clock on the cartridge (Pokemon Ruby, Sapphire and Emerald, Boktai) get one that
reads the computer's clock. `--rtc off` removes it, `--rtc on` adds it to any game, and `--rtc-start T`
starts it at Unix time T and advances it with game time, so scripted runs repeat exactly.

Save files live in the per-user data folder, under `saves/<title>-<hash>.sav`:

| Host | Folder |
|---|---|
| Windows | `%APPDATA%\anyport\anygba\saves\` |
| Linux | `~/.local/share/anyport/anygba/saves/` |
| macOS | `~/Library/Application Support/anyport/anygba/saves/` |

The environment variable `ANYGBA_DATA_DIR` replaces the per-user data folder (saves, input mapping and
settings), for a portable install or an isolated test run.

`--save-dir DIR` uses another folder; `--no-save` disables saving. A save is written about half a second
after the game stops changing it, and again on exit. Each write goes to a temporary file first, and the
previous save is kept as `<name>.sav.bak`. A save file whose size does not fit the chip stops the program
with an error instead of being overwritten. The `.sav` files are raw chip contents, the same layout mGBA
and most emulators use.

## Controls

Defaults: arrows = d-pad, Z = A, X = B (as in VisualBoyAdvance), Return = Start, Backspace = Select, A = L, S = R, Space =
fast-forward; on a gamepad, the face buttons by position (South = A, East = B), Back = Select, Start,
shoulders, the d-pad and the left stick. Hotkeys are listed above. To change them, use the menu's
Controls page or see [INPUT_MAPPING.md](INPUT_MAPPING.md).

## When something is missing

Code the static analysis could not find runs in an interpreter (slower, but correct) and is listed in
`anygba-miss.log` next to where you started the program. Rebuild with it to make that code native:

```sh
anygba build game.gba --miss-log anygba-miss.log
```

An unknown BIOS call, undefined instruction, or unsupported hardware access stops the program with a
message naming the address. Some of these can be worked around in the per-game config.

## Building step by step

`anygba build` runs the steps below; you can run them yourself to inspect or edit the generated C:

```sh
<prefix>/bin/anygba-recomp game.gba --output game-src [--config game.toml]
cmake -S game-src -B game-src/out -DCMAKE_PREFIX_PATH=<prefix>
cmake --build game-src/out --config Release
game-src/out/Release/game game.gba
```
