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
| `common/` | `anygba_common` | SHA-256 (ties generated code to its ROM), embedded-ROM trailer |
| `runtime/` | `anygba_runtime` (`anygba::runtime`, file `anygba`) | `libanygba`: `System` (bus, IO, scheduler, IRQ, DMA, timers, dispatch, fallback interpreter); C ABI for generated code in `include/anygba/cpu.h` |
| `runtime/src/save/`, `input_map.cpp` | `anygba_runtime` | Backup memory (SRAM, Flash, EEPROM), save files, `anygba-input.ini` |
| `runtime/src/main.cpp` | `anygba_game_main` (`anygba::game_main`) | `main()` for generated games |
| `tools/recomp/decode/` | `anygba_decode` | ARMv4T ARM/Thumb decoder (`Insn`) and Capstone-syntax disassembler; shared with the runtime interpreter |
| `tools/recomp/analysis/` | `anygba_recomp_lib` | ROM image with RAM regions, CFG walker with constant tracking, jump tables, function discovery |
| `tools/recomp/config/`, `io/` | `anygba_recomp_lib` | Per-game TOML config, runtime miss-log reader ([GAME-CONFIG.md](GAME-CONFIG.md)) |
| `tools/recomp/codegen/` | `anygba_recomp_lib` | C emitter (`c_emitter`, `arm_ops`, `thumb_ops`) and project writer (`dispatch_table`) |
| `tools/recomp/src/` | `anygba_recomp_lib`, `anygba-recomp` | CLI and pipeline |
| `tools/anygba/` | `anygba_driver_lib`, `anygba` | `anygba build`: recompile, compile against the install tree, embed the ROM, package |
| `cmake/anygba_game.cmake` | | `anygba_add_game()`: recompile a ROM inside this build (used by the corpus tests) |
| `tests/` | `anygba-tests`, `platform-smoke` | doctest unit tests by module, smoke app, corpus lock and fetch scripts |
| `tests/oracle/` | | Scripts that produce reference data with mGBA (not built; see its README) |
| `.github/workflows/ci.yml` | | Build and test matrix |

Language levels: C++20 for tools and runtime, C11 for generated game code. `runtime.h` and `cpu.h` are C
headers so generated code can include them.

## Generated code

Each discovered function becomes `void f_XXXXXXXX(anygba_cpu* c)`. Registers live in `c->r[16]`, flags in
separate `n/z/c/v` fields, and PC reads are constants. Inside a function, branches to its own labels are
`goto`s and conditional instructions are `if` blocks. Memory goes through inline helpers in `cpu.h` with
a page-table fast path for EWRAM, IWRAM and ROM; everything else calls the bus. PC-relative word loads from
ROM are folded to constants.

Control flow between functions:

- `BL` stores LR, pushes the return address on `c->ret_stack`, and calls the target directly
  (`AG_CALL`). A function always returns with `r[15]` set to where execution continues.
- After a call, if `r[15]` is not the expected return address, `anygba_resume` keeps dispatching from
  `r[15]` (recompiled function, else the interpreter) until it is, or returns 1 when an outer call site
  expects that address, and the frames return up to it. Tail calls, `bx` veneers, and non-standard
  returns all go through this path; `anygba_unwind` (longjmp to `System::run`) is only for context
  switches and an overflowing return stack.
- Indirect jumps set `r[15]` and return, except bounded jump tables, which become a `switch` of
  `goto`s, and jumps with LR already pointing at the next instruction, which are indirect calls.
- A branch to itself calls `anygba_idle_loop`, which skips to the next scheduled event.

Cycle accounting: each instruction costs the WAITCNT-derived fetch cost of its region plus a small extra
for loads, multiplies and block transfers. Costs are batched and charged at labels and before control
flow, where `AG_TICK` calls `anygba_tick` once the budget to the next event is spent. IRQs are delivered
there synchronously, the way the BIOS vector does it.

`write_project` writes `game.h`, `funcs_NNN.c` shards (500 functions each by default), `dispatch.c`
(sorted function table plus `anygba_game_info` with the ROM's SHA-256) and a `CMakeLists.txt` that finds
an installed runtime. The game executable refuses a ROM whose hash differs.

## Runtime

`anygba::System` owns the CPU context, memory, and timeline. Memory map and quirks follow GBATEK:
mirrors, VRAM's 32 KiB mirror, byte writes to palette (doubled), VRAM (BG only) and OAM (ignored), SRAM's
8-bit bus, ROM open bus past the image. The scheduler counts cycles down to the next event (HBlank,
line end, timer overflows); video timing is 1232 cycles per line and 228 lines per frame. DMA channels
run instantly on their trigger; timers are read lazily and cascade. Errors the runtime will not guess
about (unknown SWI, undefined instruction) stop `System::run` with `anygba::Error`.

### Saves

`runtime/src/save/` implements the cartridge backup chips behind one `save::Backup` interface; `System`
picks the type from `Options::save_type`, then the generated game's config value, then the ROM's library ID
string. The bus sends 0x0E/0x0F bytes to it. With an EEPROM, page 0x0D leaves the fast path and halfword
accesses there (all of 0x0D on carts up to 16 MB, 0x0DFFFF00+ on 32 MB carts) reach the serial port;
`run_dma` passes the DMA 3 length along so the first request fixes the address width (6 or 14 bits).
Flash erase and program finish instantly. `save::SaveFile` (used by `main.cpp`) persists `Backup::data()`
after 30 unchanged frames and on exit: write `.tmp`, copy the old file to `.bak`, rename.

### HLE BIOS

`runtime/src/bios/` replaces the BIOS ROM. `swi_table.cpp` dispatches SWI 0x00-0x2A; numbers past the
table stop the run with an error naming the number and address. The functions read r0-r3, leave results
and documented register side effects (Div: r0 quotient, r1 remainder, r3 absolute quotient; ArcTan2:
r3 = 0x170), and charge an approximate cycle cost. Halt, Stop, IntrWait and VBlankIntrWait skip time to the
next interrupt. `boot_state.cpp` holds the post-boot state (stack pointers per mode, SYS mode, DISPCNT
forced blank, identity BG2/BG3 matrices, SOUNDBIAS 0x200, POSTFLG 1) and SoftReset, RegisterRamReset and
HardReset. BIOS-region reads return the opcode the real BIOS would have prefetched last: after boot, after
a SWI, inside an IRQ handler, and after one. BgAffineSet/ObjAffineSet use the BIOS's 1.14 sine table in
fixed point. The sound-driver SWIs and MultiBoot are stubs that log once. Reference data for the tests is
described in `tests/runtime/data/bios/README.md` and `tests/oracle/README.md`.

### PPU

`runtime/src/ppu/` renders one line at the start of each visible line's HBlank from the current IO
registers, VRAM, OAM and palette into `System::framebuffer()` (RGB555, the GBA's own format), so HBlank
DMA and VCOUNT-IRQ register changes apply from the next line. Text backgrounds (`bg_text`), affine
backgrounds (`bg_affine`) and bitmap modes 3-5 (`bg_bitmap`, which also use BG2's matrix) fill one line
buffer per BG; `sprites` fills the OBJ line (priority, semi-transparency flag) and the OBJ-window mask
with the per-line OBJ cycle budget; `compose` applies windows, picks the two top layers, and blends.
BG2/BG3 internal reference points are reloaded at VBlank and when BGxX/BGxY is written, and advance by
PB/PD after every line. Golden pictures from mGBA (`tests/golden/ppu/`) check the result.

### APU

`runtime/src/apu/` produces 32768 Hz stereo (the GBA's own output rate; SDL resamples to the device).
Output is generated lazily: register writes and timer overflows first call `catch_up(now)`, which emits
every 512-cycle sample due so far, so sample timing does not depend on when the CPU ticks. Each output
sample averages the PSG channels over its 512 cycles (`psg`), FIFOs pop on timer 0/1 overflow and ask
DMA 1/2 for 16 bytes when half empty (`fifo`), and `mixer` applies GBATEK's volumes, SOUNDBIAS and the
10-bit clip. `System::drain_audio` hands samples to `main.cpp`, which paces emulation by the queue.

The fallback interpreter (`runtime/src/interp/`) shares only the decoder with the recompiler. It runs
code that has no recompiled function and is the oracle for the differential tests. It logs the address
where it starts and every call target it reaches without recompiled code, so one feedback pass covers
code that only ever ran inside it.

`runtime/src/main.cpp` is the host loop for generated games, driven from VBlank: present the framebuffer,
feed the audio stream (silence until the APU exists) and wait while more than two frames of audio are
queued, so the sound card's clock sets the speed; Tab skips the wait. Headless runs use `FramePacer`
at 59.7275 Hz when `--pace` is given.

## Install package

`cmake/install.cmake` installs `anygba-recomp`, the runtime libraries (`anygba::runtime`,
`anygba::game_main`, `anygba::platform`, `anygba::decode`, `anygba::common`), their headers, SDL3 (static,
through SDL's own install rules), and `lib/cmake/anygba/anygbaConfig.cmake`. The `CMakeLists.txt` that
anygba-recomp writes does `find_package(anygba)` against that prefix. Release archives are this install
tree.

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
- toml++ 3.4.0 (MIT), single header, recompiler config parsing.
- doctest 2.5.3 (MIT), single header, tests only.
- Capstone 5.0.6 (BSD-3-Clause), ARM only, tests only: the disassembler oracle. Nothing shipped links it.

To build against a local checkout, set `FETCHCONTENT_SOURCE_DIR_SDL3` or `FETCHCONTENT_SOURCE_DIR_DOCTEST`.

## Tests

- `anygba-tests`: doctest unit tests grouped by module under `tests/platform`, `tests/recomp`,
  `tests/runtime`. Platform tests run on the headless context, so they need no display or sound card.
  - Decoder: exhaustive Thumb and class-sampled ARM against Capstone.
  - Discovery: hand-assembled ROMs, plus armwrestler against its ELF mapping symbols.
  - Codegen: about 13,000 sampled instructions are emitted as C at build time
    (`anygba-codegen-fixture`), then each runs from 80 random register/flag/mode/memory states through
    both the generated C and the interpreter, and the results must match exactly.
  - Interpreter: jsmolka arm, thumb and memory with no recompiled code.
- `gba-tests-arm`, `gba-tests-thumb`, `gba-tests-memory`: the ROMs recompiled by `anygba_add_game`, run
  with `--headless --exit-on-idle`; they pass when the ROM reaches its idle loop with r12 = 0.
- `platform-smoke --headless`: end-to-end check of window scaling, audio drain, and keyboard routing.
- `install-runtime` + `feedback-irq_demo`, `feedback-dma_demo` (label `slow`): install the package, then
  `tests/feedback-pass.cmake` recompiles a tonc demo, builds it against the package, runs it headless,
  recompiles with the miss log, and fails if the second run still interprets anything.
- `varooom-3d`, `tonc-*` targets (not built by default): larger generated programs for manual checks.
- Corpus: `tests/corpus.lock` lists `kind sha256 path url` per line. `file` entries are saved as-is;
  `zip` entries are verified, cached under `.archives/`, and extracted into a directory stamped with the
  archive hash. Butano publishes no prebuilt example ROMs, so the Butano entry is the official Varooom 3D
  release build; building the Butano examples needs devkitARM and is not part of the corpus. Corpus
  tests are skipped (with a message) when the corpus is not fetched; fetch it before configuring so the
  ROM tests are generated.

## CI

`.github/workflows/ci.yml` fetches and verifies the corpus with the script native to each host, then
builds Release and runs CTest on Windows (MSVC and clang-cl), Ubuntu (GCC and Clang), and macOS arm64.
`.github/workflows/release.yml` packages tagged versions (see the README).
