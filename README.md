# AnyGBA

AnyGBA is a static recompiler that turns a Game Boy Advance ROM into a native Windows, Linux, or macOS
executable. The ROM's ARM/Thumb code is translated to C ahead of time and linked against `libanygba`, a
high-level runtime that provides the BIOS, video, audio, and I/O. There is no emulator loop; an
interpreter exists only as a fallback for code the recompiler missed.

Status: early bring-up.

What works:

- Decoder, function discovery, C code generator, and runtime core (bus, scheduler, interrupts, DMA,
  timers, fallback interpreter). jsmolka's arm, thumb, memory and bios tests pass as recompiled native code.
- HLE BIOS: every GBA SWI from 0x00 to 0x2A. Math, memory copy, decompression and affine functions are
  native; Div, Sqrt and ArcTan/ArcTan2 match mGBA's HLE BIOS bit for bit on 640 inputs, and the
  decompressors reproduce data packed by grit. Unknown SWI numbers stop the game with an error.

What does not work yet:

- No video and no audio, so games run but show nothing.
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
<prefix>/bin/anygba-recomp game.gba --output game-src [--config game.toml]
cmake -S game-src -B game-src/out -DCMAKE_PREFIX_PATH=<prefix>
cmake --build game-src/out --config Release
game-src/out/<config>/game game.gba          # window; --headless for none
```

Code the static analysis missed runs in the interpreter and is listed in `anygba-miss.log`. Recompile
with `--miss-log anygba-miss.log` and rebuild to turn it into native code.

## Releases

Pushing a tag `v*` runs `.github/workflows/release.yml`, which builds the package on Windows x64, Linux
x64 and macOS arm64, checks that each one can build a generated project, and attaches the archives to a
GitHub Release.

## Programs

- `anygba-recomp <rom.gba> [--config game.toml] [--miss-log FILE]... [--output dir] [--list]`: recompiler
  CLI. Writes a C project to the output directory; `--list` prints the discovered functions with
  disassembly instead. Config schema and the miss-log format: [docs/dev/GAME-CONFIG.md](docs/dev/GAME-CONFIG.md).
- A generated game: `game <rom.gba> [--headless] [--pace | --fast] [--frames N] [--seconds S]
  [--exit-on-idle] [--miss-log FILE | --no-miss-log] [--scale N]`. It only accepts the ROM it was generated
  from (SHA-256). With a window it paces to the audio device and Tab fast-forwards; headless runs are
  unpaced unless `--pace`. Addresses it had to interpret are appended to `anygba-miss.log`.
- `platform-smoke`: opens a window with a test pattern and plays a 440/660 Hz tone. Held buttons light up
  along the bottom; Escape quits. `--headless` runs on SDL's dummy drivers, checks the scaled output, the
  audio drain, and keyboard routing, and exits non-zero on a mismatch. CTest runs it this way.

Default keys: arrows = d-pad, X = South (A), Z = East (B), A = West, S = North, Q/W = shoulders,
Return = Start, Backspace = Back. Any SDL gamepad works as well.

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
