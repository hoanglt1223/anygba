# AnyGBA

AnyGBA is a static recompiler that turns a Game Boy Advance ROM into a native Windows, Linux, or macOS
executable. The ROM's ARM/Thumb code is translated to C ahead of time and linked against `libanygba`, a
high-level runtime that provides the BIOS, video, audio, and I/O. There is no emulator loop; an
interpreter exists only as a fallback for code the recompiler missed.

## Why this exists

I grew up with the Game Boy Advance. In primary school, the games I couldn't afford I played on a family
PC through VisualBoyAdvance: Pokémon, a cheat code scribbled in a notebook, a save state before every hard
fight. Those evenings are a big part of why I write software today.

AnyGBA is my way of giving something back to that childhood. I want to bring those games back to life,
make them run better than they ever did on that old PC (native code, sharp at 4K, instant saves), make up
for what that kid couldn't have, and carry the feeling forward for the next person who opens their old
cartridges. VBA kept these games alive for a generation; this project hopes to carry them a little further.

> *Mình làm AnyGBA vì tuổi thơ, vì tình yêu với GBA và VBA từ những ngày tiểu học: để hồi sinh, cải thiện,
> bù đắp và tiếp nối tuổi thơ ấy.*

Status: every plan phase is implemented and tested on Windows, including one commercial title from the
user's own cartridge dump (Pokemon Emerald reaches gameplay and saves); Linux/macOS CI is still to come.
Compatibility: [compat/](compat/) (page: `python tools/compat-site/generate.py`).

What works:

- Decoder, function discovery, C code generator, and runtime core (bus, scheduler, interrupts, DMA,
  timers, fallback interpreter). jsmolka's arm, thumb, memory and bios tests pass as recompiled native code.
- HLE BIOS: every GBA SWI from 0x00 to 0x2A. Math, memory copy, decompression and affine functions are
  native; Div, Sqrt and ArcTan/ArcTan2 match mGBA's HLE BIOS bit for bit on 640 inputs, and the
  decompressors reproduce data packed by grit. Unknown SWI numbers stop the game with an error.
- PPU: scanline renderer for all six video modes, 128 regular and affine sprites, windows (including
  the OBJ window), alpha blending, brightness, mosaic, and green swap. 48 test pictures from the tonc
  demos, jsmolka's PPU tests and Varooom 3D match mGBA pixel for pixel, including an HBlank DMA raster
  effect; the only accepted differences are documented rounding differences in the reference itself.
- APU: the four PSG channels (sweep, envelopes, length, wave banks, 7/15-bit noise) and both
  DirectSound FIFOs fed by DMA, mixed per GBATEK at 32768 Hz. Self-authored test tones correlate with
  mGBA's output above 0.99 (noise 0.98, a panned mix 0.988); tonc's snd1_demo and a Varooom 3D sound
  effect match as well. Generated games print a count of DirectSound FIFO underruns on exit.
- Saves: SRAM, Flash 64/128 KB and EEPROM 512 B/8 KB, detected from the ROM or set per game, written to
  the per-user data folder with a backup of the previous file. jsmolka's save tests pass and keep their
  save across restarts.
- 41 open-source titles (34 tonc demos, Varooom 3D, ARMWrestler, jsmolka's PPU/NES/unsafe tests) run
  60 s as recompiled code without errors, well above full speed, with the same final picture as the
  interpreter (except tonc `txt_se2`, which prints its own timing measurements); their frame hashes
  are CI checkpoints.
- `anygba build game.gba`: one command from ROM to a program with the ROM embedded
  ([docs/user/USAGE.md](docs/user/USAGE.md)). Controls come from `anygba-input.ini`
  ([docs/user/INPUT_MAPPING.md](docs/user/INPUT_MAPPING.md)).
- VisualBoyAdvance-style player features in every generated game:
  - Save states in nine slots (Shift+F1-F9 save, F1-F9 load). A state saved and loaded later, in the
    same run or a new one, continues with the same frames, even though the game is native code
    ([docs/dev/SAVE-STATES.md](docs/dev/SAVE-STATES.md)).
  - Speed 100/200/300/400% with the sound resampled instead of broken up, plus a hold-to-fast-forward key.
  - Cheats: GameShark v1/v2, Action Replay v3 (GameShark SP), CodeBreaker and raw codes, encrypted
    forms included, with conditions, button codes and ROM patches. They come from an in-game menu with
    categories and value pickers, typed codes, a per-game cheat file, or the command line
    ([docs/user/CHEATS.md](docs/user/CHEATS.md)). A curated Pokemon Emerald database ships with AnyGBA;
    `tools/cheats/import-libretro.py` converts the libretro community lists for about 490 more games.
  - An in-game menu (Escape or Back+Start) for states, speed, cheats and rebinding keys and gamepad
    buttons; on-screen messages; gamepad hot-plug with SDL's controller database and the left stick.
- AI agents can play and debug a running game through `anygba-mcp`, an MCP server shipped in the package
  ([docs/user/AI-CONTROL.md](docs/user/AI-CONTROL.md)): screenshots, frame-accurate input, pause and step,
  save states, cheats, memory, named game variables (a Pokemon Emerald symbols file ships), breakpoints at
  function entries and memory watches. A play journal records every agent command, your own actions,
  snapshots and thumbnails so later sessions can learn from earlier ones. Both are off by default, can be
  switched on and off while playing (menu Settings, F10, F11), listen on 127.0.0.1 with a per-user token,
  and give way as soon as you press a button.

What does not work yet:

- Audio and windows: never run on a real sound device or window on the development PC (headless
  only), so windowed playback, real keyboard/gamepad input, audio-led pacing at every speed and the
  drawn menu are untested; the same code paths run headless with scripted input in the tests.
- The real-time clock some cartridges have (Pokemon Ruby, Sapphire, Emerald) is not emulated: those
  games report a dry battery and skip time-based events.
- EEPROM is checked by unit tests only; no open-source test ROM in the corpus uses it.
- The macOS `.app` bundle and its ad-hoc signing are written but have not been run on a Mac.
- The sprite-per-line cycle limit is implemented but no test content reaches it.
- The BIOS sound driver (SWI 0x1A-0x1E, 0x20-0x24, 0x28-0x2A) and MultiBoot are stubs that log once.
  Games that use their own sound engine (most of them) do not call these.

## Legal

AnyGBA ships only the tool and the runtime. You supply your own ROM dumps. No ROM, BIOS image, or
copyrighted asset is part of this repository or required by it; the BIOS is reimplemented in the runtime.

## Build

Requirements: CMake 3.28 or newer, a C++20 compiler (MSVC 2022, clang-cl, GCC 13+, Clang 16+, or Apple
Clang 15+), and network access on the first configure. SDL 3.4.18, toml++ 3.4.0, and (for tests) doctest
2.5.3 and Capstone 5.0.6 are downloaded by CMake at pinned versions and verified by SHA-256.

```sh
sh tests/fetch-corpus.sh            # optional, before configuring: enables the ROM tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure           # everything
ctest --test-dir build --build-config Release --output-on-failure -LE slow  # skip the feedback-pass tests
cmake --install build --config Release --prefix <prefix>                     # recompiler + runtime package
```

On Windows the default generator is Visual Studio; add `-T ClangCL` for clang-cl. Linux needs the usual
SDL development packages (see `.github/workflows/ci.yml` for the list).

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `ANYGBA_BUILD_TESTS` | ON at top level | Build `anygba-tests` and `platform-smoke` |
| `ANYGBA_WARNINGS_AS_ERRORS` | OFF | Treat warnings in project targets as errors |
| `ANYGBA_SANITIZE` | empty | `address`, `undefined`, or `address;undefined` (MSVC: `address` only) |

## Recompiling a ROM

With an installed package (built as above, or a release archive):

```sh
<prefix>/bin/anygba build game.gba [--config game.toml] [-o dist]   # dist/<Title>.exe, ROM embedded
```

Code the static analysis missed runs in the interpreter and is listed in `anygba-miss.log`. Rebuild
with `--miss-log anygba-miss.log` to turn it into native code. Saves, controls, and the separate
recompile/configure/build steps: [docs/user/USAGE.md](docs/user/USAGE.md).

For Android, `--target android` writes a signed APK and `--target android-package` a game package for
the AnyGBA launcher app: [docs/user/ANDROID.md](docs/user/ANDROID.md).

## Releases

Pushing a tag `v*` runs `.github/workflows/release.yml`, which builds the package on Windows x64, Linux
x64 and macOS arm64, checks that each one can build a generated project, and attaches the archives to a
GitHub Release, together with the Android runtime archive and the launcher APK.

## Programs

- `anygba-recomp <rom.gba> [--config game.toml] [--miss-log FILE]... [--overrides DIR]... [--symbols FILE]...
  [--output dir] [--list]`: recompiler CLI. Writes a C project to the output directory; `--list` prints the
  discovered functions with disassembly instead. Config schema and the miss-log format:
  [docs/dev/GAME-CONFIG.md](docs/dev/GAME-CONFIG.md); function overrides: [docs/dev/OVERRIDES.md](docs/dev/OVERRIDES.md).
- `anygba build <rom.gba> [options]`: recompile, compile and package in one step (`anygba help build`).
- `anygba-mcp [--control-dir DIR] [--journal-dir DIR]`: MCP server on stdin/stdout for AI agents
  ([docs/user/AI-CONTROL.md](docs/user/AI-CONTROL.md); protocol: [docs/dev/control-protocol.md](docs/dev/control-protocol.md)).
- A generated game: `game [rom.gba] [--headless] [--pace | --fast] [--speed N] [--frames N] [--seconds S]
  [--exit-on-idle] [--dump-frame-hash] [--save-dir DIR | --no-save] [--save-type T] [--load-state FILE]
  [--cheat CODE] [--cheats FILE] [--cheat-db FILE] [--input FILE] [--script FILE] [--keys SPEC]
  [--dump REGION:FILE] [--miss-log FILE | --no-miss-log] [--scale N] [--control] [--journal] [--symbols FILE]
  [--overrides on|off|all]
  ...` (`--help` explains each). It only
  accepts the ROM it was generated from (SHA-256); without a ROM argument it uses the one `anygba build`
  embedded. With a window it paces to the audio device and Space fast-forwards; headless runs are unpaced
  unless `--pace`. Addresses it had to interpret are appended to `anygba-miss.log`.
- `platform-smoke`: opens a window with a test pattern and plays a 440/660 Hz tone. Held buttons light up
  along the bottom; Escape quits. `--headless` runs on SDL's dummy drivers, checks the scaled output, the
  audio drain, and keyboard routing, and exits non-zero on a mismatch. CTest runs it this way.

Default keys in `platform-smoke`: arrows = d-pad, X = South (A), Z = East (B), A = West, S = North,
Q/W = shoulders, Return = Start, Backspace = Back. Any SDL gamepad works as well. Games use
`anygba-input.ini` ([docs/user/INPUT_MAPPING.md](docs/user/INPUT_MAPPING.md)).

## Test corpus

Test ROMs are open-source homebrew, downloaded on demand and never committed. `tests/corpus.lock` pins
every URL and SHA-256.

```sh
sh tests/fetch-corpus.sh            # Linux, macOS, Git Bash
pwsh tests/fetch-corpus.ps1         # Windows (Windows PowerShell 5.1 works too)
```

Both scripts download into `tests/corpus/` (override with `--dest DIR` / `-Dest DIR`), skip files that
already match, and exit non-zero if any download fails or any hash differs.

| Set | Source |
|---|---|
| gba-tests | [jsmolka/gba-tests](https://github.com/jsmolka/gba-tests) (arm, thumb, memory, bios, nes, ppu, save, unsafe) |
| armwrestler | [destoer/armwrestler-gba-fixed](https://github.com/destoer/armwrestler-gba-fixed) (ROMs plus ELF) |
| tonc | [Tonc](https://www.coranac.com/tonc/text/) demo binaries (`tonc-bin.zip`) |
| butano | [Varooom 3D](https://github.com/GValiente/butano/releases/tag/7.3.0), a Butano release build |

## Layout and design

See [docs/dev/ARCHITECTURE.md](docs/dev/ARCHITECTURE.md).

## License

MIT, see [LICENSE](LICENSE).
