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
- Hold Tab (configurable) to fast-forward.
- `--headless --frames N --dump-frame-hash`: no window or audio; prints the SHA-256 of the last frame.
  Used by tests and the compatibility tracker.
- `--exit-on-idle`: stop at the first branch-to-self (test ROMs end this way) and print the registers.

## Saves

The backup memory type is read from the ROM's library ID string (`SRAM_V`, `FLASH_V`, `FLASH512_V`,
`FLASH1M_V`, `EEPROM_V`). A per-game config (`[game] save = "..."`) or `--save-type` overrides it.
EEPROM size (512 bytes or 8 KB) is taken from the game's first EEPROM transfer, or from an existing save
file.

Save files live in the per-user data folder, under `saves/<title>-<hash>.sav`:

| Host | Folder |
|---|---|
| Windows | `%APPDATA%\anyport\anygba\saves\` |
| Linux | `~/.local/share/anyport/anygba/saves/` |
| macOS | `~/Library/Application Support/anyport/anygba/saves/` |

`--save-dir DIR` uses another folder; `--no-save` disables saving. A save is written about half a second
after the game stops changing it, and again on exit. Each write goes to a temporary file first, and the
previous save is kept as `<name>.sav.bak`. A save file whose size does not fit the chip stops the program
with an error instead of being overwritten. The `.sav` files are raw chip contents, the same layout mGBA
and most emulators use.

## Controls

Defaults: arrows = d-pad, X = A, Z = B, Return = Start, Backspace = Select, A = L, S = R, Space =
fast-forward; on a gamepad, the face buttons by position (South = A, East = B), Back = Select, Start,
shoulders, and the d-pad. To change them, see [INPUT_MAPPING.md](INPUT_MAPPING.md).

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
