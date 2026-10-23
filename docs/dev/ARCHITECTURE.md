# Architecture

AnyGBA has two halves. `anygba-recomp` reads a ROM once and writes a C project. That project links
against `libanygba`, which emulates the hardware at a high level, and the host compiler produces a
native executable.

```
ROM ──► anygba-recomp (C++20)                       libanygba (C++20, SDL3)
        ├─ ARMv4T/Thumb decoder                     ├─ bus + IO registers + wait states
        ├─ function/CFG discovery + trace feedback   ├─ scheduler (cycle budget, IRQ, DMA, timers)
        └─ C code generator  ──► game_funcs.c ──┐    ├─ HLE BIOS
                                                ├──► ├─ PPU scanline renderer ─► platform window
                          dispatch table .c ────┘    ├─ APU ─► platform audio
                                                     ├─ saves, input, config
                                                     └─ fallback interpreter + miss logger
```

## Repository layout

| Path | Target | Contents |
|---|---|---|
| `cmake/` | | `toolchain` (language levels, output dirs, MSVC flags), `warnings`, `sanitizers`, `dependencies` (pinned FetchContent) |
| `platform/` | `anyport_platform` (`anyport::platform`) | Host layer on SDL3: context, window, audio, input, timing, log |
| `runtime/` | `anygba_runtime` (`anygba::runtime`, file `anygba`) | `libanygba`; C interface in `include/anygba/runtime.h` |
| `tools/recomp/` | `anygba_recomp_lib`, `anygba-recomp` | Recompiler library and CLI |
| `tests/` | `anygba-tests`, `platform-smoke` | doctest unit tests by module, smoke app, corpus lock and fetch scripts |
| `.github/workflows/ci.yml` | | Build and test matrix |

Language levels: C++20 for tools and runtime, C11 for generated game code. `runtime.h` is a C header so
generated code can include it.

## Platform layer

`platform/` is shared with other anyport projects (AnySwitch first), so it holds no GBA knowledge. Its
namespace is `anyport::platform` and its headers live under `include/anyport/platform/`. Errors are
reported by throwing `anyport::platform::Error` with the failing SDL call and `SDL_GetError()` text.

| Header | API |
|---|---|
| `context.h` | `Context`: owns `SDL_Init` for video, audio, gamepads. One at a time; destroy other platform objects first. `headless` selects SDL's dummy video and audio drivers and the software renderer. |
| `window.h` | `Window`: resizable window showing one framebuffer through a streaming texture, nearest-neighbour filtering, and SDL's integer-scale logical presentation (largest whole factor that fits, black letterbox). Formats: `PixelFormat::Rgb555` (red in bits 0-4). `present()` can read the composed output back as RGBA for tests. |
| `audio.h` | `AudioStream`: signed 16-bit interleaved stream at any source rate and 1-8 channels; SDL resamples to the device. `push()` takes whole frames; `queued_frames()` drives audio-led pacing. |
| `input.h` | `Input`: virtual pad in SDL gamepad terms (South/East/West/North, d-pad, shoulders, triggers as buttons above half travel, sticks), merged from the keyboard and all connected gamepads. Keys bind by SDL key name. `pump_events()` feeds it and reports quit. |
| `timing.h` | `now_ns`, `sleep_ns`, and `FramePacer`, which never returns early and restarts its schedule after falling more than one period behind. |
| `log.h` | printf-style `log(level, ...)` on SDL's log. |

Mapping virtual pad buttons to a guest console, the guest frame rate, and the guest sample rate all
belong to the project using the layer (for AnyGBA: 240x160 RGB555, 32768 Hz stereo, ~59.73 Hz).

## Dependencies

All third-party code arrives through `cmake/dependencies.cmake` at an exact release with a SHA-256:

- SDL 3.4.18 (zlib), built static.
- doctest 2.5.3 (MIT), single header, tests only.

To build against a local checkout, set `FETCHCONTENT_SOURCE_DIR_SDL3` or `FETCHCONTENT_SOURCE_DIR_DOCTEST`.

## Tests

- `anygba-tests`: doctest unit tests grouped by module under `tests/platform`, `tests/recomp`,
  `tests/runtime`. Platform tests run on the headless context, so they need no display or sound card.
- `platform-smoke --headless`: end-to-end check of window scaling, audio drain, and keyboard routing.
- Corpus: `tests/corpus.lock` lists `kind sha256 path url` per line. `file` entries are saved as-is;
  `zip` entries are verified, cached under `.archives/`, and extracted into a directory stamped with the
  archive hash. Butano publishes no prebuilt example ROMs, so the Butano entry is the official Varooom 3D
  release build; building the Butano examples needs devkitARM and is not part of the corpus.

## CI

`.github/workflows/ci.yml` builds Release and runs CTest on Windows (MSVC and clang-cl), Ubuntu (GCC and
Clang), and macOS arm64, then fetches and verifies the corpus with the script native to each host.
