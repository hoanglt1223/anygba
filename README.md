# AnyGBA

AnyGBA is a static recompiler that turns a Game Boy Advance ROM into a native Windows, Linux, or macOS
executable. The ROM's ARM/Thumb code is translated to C ahead of time and linked against `libanygba`, a
high-level runtime that provides the BIOS, video, audio, and I/O. There is no emulator loop; an
interpreter exists only as a fallback for code the recompiler missed.

Status: early bring-up. The build, platform layer, CI, and test corpus are in place. The recompiler and
runtime are skeletons.

## Legal

AnyGBA ships only the tool and the runtime. You supply your own ROM dumps. No ROM, BIOS image, or
copyrighted asset is part of this repository or required by it; the BIOS is reimplemented in the runtime.

## Build

Requirements: CMake 3.28 or newer, a C++20 compiler (MSVC 2022, clang-cl, GCC 13+, Clang 16+, or Apple
Clang 15+), and network access on the first configure. SDL 3.4.18 and doctest 2.5.3 are downloaded by
CMake at pinned versions and verified by SHA-256.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

On Windows the default generator is Visual Studio; add `-T ClangCL` for clang-cl. Linux needs the usual
SDL development packages (see `.github/workflows/ci.yml` for the list).

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `ANYGBA_BUILD_TESTS` | ON at top level | Build `anygba-tests` and `platform-smoke` |
| `ANYGBA_WARNINGS_AS_ERRORS` | OFF | Treat warnings in project targets as errors |
| `ANYGBA_SANITIZE` | empty | `address`, `undefined`, or `address;undefined` (MSVC: `address` only) |

## Programs

- `anygba-recomp <rom.gba> [--config game.toml] [--output dir]`: recompiler CLI (skeleton; it validates
  its inputs and then reports that the pipeline is not implemented yet).
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
