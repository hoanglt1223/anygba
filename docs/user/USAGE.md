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
| `--config FILE` | per-game config: entry points, RAM code, save type, patches, video ([GAME-CONFIG.md](../dev/GAME-CONFIG.md)); default: the one shipped for the ROM's game code in `share/anygba/configs/`, if any |
| `--miss-log FILE` | trace-feedback log from an earlier run (repeatable) |
| `--overrides DIR` | override module: C++ that replaces chosen game functions ([OVERRIDES.md](../dev/OVERRIDES.md); repeatable) |
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
- `--hd-pack DIR`, `--hd-dump DIR`, `--hd-dump-filter F`, `--ai-device D`: HD packs, art dumps and AI
  upscaling ([HD-PACKS.md](HD-PACKS.md)); `--video widescreen=16:9` for widescreen
  ([WIDESCREEN.md](WIDESCREEN.md)).
- `--screenshot N:FILE`, `--screenshot-size WxH`: save frame N as a PNG as the window would show it
  (filters, HD and widescreen included), also with `--headless`.
- `--headless --frames N --dump-frame-hash`: no window or audio; prints the SHA-256 of the last frame.
  Used by tests and the compatibility tracker.
- `--exit-on-idle`: stop at the first branch-to-self (test ROMs end this way) and print the registers.
- `--dump REGION:FILE`: write memory (`ewram`, `iwram`, `io`, `palette`, `vram`, `oam`) or the last
  picture (`frame`, 240x160 RGB555) to a file on exit. Handy for finding cheat addresses.
- `--wav FILE`: record the sound as it is played (after speed changes) to a WAV file, also headless.
- `--overrides on|off|all`: function overrides built into the game; `off` runs the original code for
  comparison ([OVERRIDES.md](../dev/OVERRIDES.md)).
- `--difftest FUNC [--spec FILE] [--states N] [--recorded FILE]...` and `--record-entry FUNC --count N
  --record-out FILE`: differential tests of an override against the original code, and recording real
  start states for them (`anygba difftest` wraps both; [OVERRIDES.md](../dev/OVERRIDES.md)).
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
| Ctrl+I | | smoothing on/off ([VIDEO.md](VIDEO.md#smoothing)) |

All of them can be changed in the Controls page of the menu or in
[INPUT_MAPPING.md](INPUT_MAPPING.md). Messages about slots, speed, cheats and gamepads appear at the bottom
left of the picture for a few seconds.

## Speed

Speed steps through 100, 200, 300 and 400%. The sound plays faster and higher with the game instead of
breaking up: each frame's sound is resampled by the speed factor, so the sound card still sets the pace.
The starting speed is `--speed`, else `Speed` in the input file's `[player]` section.

Holding fast-forward runs at the fast-forward speed: 150, 200, 300 (default), 400, 500 or 800%, with the
sound resampled the same way, or *unlimited*, which runs as fast as the computer can and drops the sound
the card cannot keep up with. Set it in the menu (Game tab), with `FastForwardSpeed` in the input file's
`[player]` section, or for one run with `--ff-speed N|unlimited`. A change in the menu is saved to the
input file.

*Normal-speed music* (Audio tab, `NormalSpeedMusic = on` in `[player]`; off by default) keeps the music
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
and GBA frames emulated per second, the monitor refresh and frame pacing ([VIDEO.md](VIDEO.md#refresh-and-frame-pacing)), the measured speed against its target (`(FF)` while fast-forward is
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

Escape (gamepad: Back+Start) pauses the game and opens the menu over a frosted copy of the picture.
Escape or the same gamepad chord closes it, and so does B on a tab's first page. The menu is drawn at the
window's full resolution in its own font (Noto Sans), in English or Vietnamese, with a dark or light theme.

- **Keyboard and gamepad**: the GBA buttons move through it. Up/Down choose a row, A acts on it, Left/Right
  change a value, B goes back. L/R (or Tab and Shift+Tab) switch tabs; Left on a row with no value moves to
  the tab rail, where Up/Down switch tabs and A or Right go back to the page. Page Up/Page Down jump in
  long lists.
- **Mouse**: click a tab or a row, the arrows of a value, or outside the menu to close it; the wheel
  scrolls.
- **Touch** (Android): tap like a click, drag to scroll; rows are taller.

The tabs:

- **Game**: *Resume*; a *Speed* section with *Speed* (100-400%), *Fast-forward (hold)* (150-800% or
  unlimited while the fast-forward key is held) and *Normal-speed music*, each showing its hotkeys; then
  *Reset* and *Exit game* (both ask for a second A; Exit writes the game's save first).
- **Save states**: the nine slots as a grid with a picture of the moment and when it was saved. A (or a
  click) on a slot offers *Load*, *Save here* and *Delete*; Select deletes. The picture is kept next to the
  state as `<state>.png`.
- **Video**: a *Graphics preset* row (Very low to Ultra, or Custom; [VIDEO.md](VIDEO.md#graphics-presets)), then filter, screen effect, ghosting, colour, gamma, debanding, resolution, fullscreen, integer
  scale and the HD and widescreen settings; they apply at once and are kept for this game ([VIDEO.md](VIDEO.md)).
  Beside the rows a live preview shows the paused game (see Video preview below) and, under it, what the
  focused setting does.
- **Audio**: *Volume* (0-100%) and *Mute*. Volume and Mute change what the sound
  card plays, not `--wav` recordings.
- **Controls**: a picture of the GBA with the selected button lit, then every GBA button and hotkey with
  its keyboard and gamepad bindings. Choose one, then press the new key (with Ctrl, Alt or Shift for a
  hotkey chord) or gamepad button (hold another gamepad button first for a chord such as Back+Start). If
  that key was bound to something else, the two swap. Escape cancels. *Reset to defaults* restores the
  defaults. Changes are saved to the input mapping file at once.
- **Cheats**: the cheat databases for this game and your own codes ([CHEATS.md](CHEATS.md)). Up on the
  first row (or a click on the field) searches every cheat by name.
- **AI**: AI control, Read-only agent and Journal switches ([AI-CONTROL.md](AI-CONTROL.md)). They take
  effect at once and last until the game ends.
- **Settings**: *Language* (English, Tiếng Việt), *Theme* (dark, light), *Menu size* (75-150%), *Reduce
  motion* (no open, close or focus animations), *FPS and stats overlay* (see Stats overlay), the save
  folder, and *Export save* / *Import save* for `.sav` files of other emulators (see Saves). They are
  saved in the input file's `[player]` section ([INPUT_MAPPING.md](INPUT_MAPPING.md)).

### Video preview

The Video tab's preview is the paused game picture split by a vertical divider: on the left (*Before*) the
plain picture, nearest-neighbour with no filter, effect or colour correction; on the right (*After*) the
picture with the current settings, drawn again each time a value changes. Both are drawn at the size the
game takes in the window and shrunk into the panel, so the loupe can show single pixels.

- **Divider**: drag it with the mouse (pressing anywhere on the picture moves it there) or with a finger
  started on it; `[` and `]` or the gamepad's left and right triggers move it a tenth at a time.
- **Loupe**: a 4x circle that follows the mouse over the picture, or a finger held on the picture (it sits
  above the finger). Select (Backspace; gamepad Back) turns it on and off for the keyboard and gamepad;
  then the right stick or Shift with the d-pad moves it.
- HD settings (internal scale, layer filter, smooth rotation, HD pack, widescreen) draw the paused frame
  again through the HD renderer. When HD was off while that frame was drawn, or with AI upscaling (its
  pictures come later from a worker), the panel says the picture shows the change once the game runs again.
  Settings that only change timing (smoothing, refresh, VRR, sync, FPS caps) and ghosting do not change a
  still picture; their descriptions say what they do.

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

To keep saves elsewhere (a OneDrive folder such as
`C:\Users\<you>\OneDrive\Documents\My Games\AnyGBA\saves`, for example), set the save folder in the menu
(Settings, *Save folder*: a folder picker) or as `SaveFolder = <folder>` in the input file's `[player]`
section; a relative folder is taken from the input file's folder. It is used from the next start for the
save file, save states, the cheat file and the video settings; move existing files there yourself.
*Use the default folder* goes back to the per-user data folder.

### Moving saves to and from other emulators

The menu's Settings tab exports and imports `.sav` files as VisualBoyAdvance and mGBA write them (the raw
chip contents). *Export save* writes `<ROM name>.sav` next to the ROM (for a program with the ROM embedded:
`<title>.sav` in the save folder); with a window, a file picker opens there so you can choose another
place. *Import save* reads a `.sav` (a picker in the same place), checks that its size fits the game's save
chip, copies the current save file to `<save>.before-import-<date>-<time>`, loads the imported save and
restarts the game. A file a little larger than the chip (VisualBoyAdvance-M's real-time clock bytes after a
128 KB Flash save, or another emulator's footer, up to 512 bytes) is cut to the chip's size; any other size
is refused with a message. From the command line: `--import-save FILE` loads one before the game starts,
`--export-save FILE` writes the save on exit.

`--save-dir DIR` uses another folder for one run (it overrides `SaveFolder`); `--no-save` disables saving. A save is written about half a second
after the game stops changing it, and again on exit. Each write goes to a temporary file first, and the
previous save is kept as `<name>.sav.bak`. When another program has the file open for a moment (OneDrive
syncing it, a virus scanner), each step is retried for about half a second before the write is reported
as failed; a failed write is tried again on the next change and on exit. A save file whose size does not fit the chip stops the program
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
