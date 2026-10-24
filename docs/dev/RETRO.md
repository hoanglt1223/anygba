# Retro: what AnyGBA hands to AnySwitch

AnyGBA was built as the learning project for AnySwitch's toolchain. This records what carried its weight
and should be reused as is, what should change on the way, and what does not transfer.

## Reuse as is

- **Build skeleton.** CMake 3.28 presets-free layout (`cmake/toolchain`, `warnings`, `sanitizers`,
  pinned `FetchContent` dependencies), MSVC/clang-cl/GCC/Clang/AppleClang matrix in `ci.yml`, a release
  workflow that installs a package and proves it can build a generated project, and an install tree with a
  CMake package so generated code links with `find_package`.
- **Platform layer** (`platform/`, `anyport::platform`): SDL3 context, window with integer scaling,
  audio stream, input with positional gamepad buttons, timing. It never needed a GBA-specific change.
- **Headless-first testing.** Every program has `--headless --frames N`; the game prints a frame hash
  with `--dump-frame-hash`. All tests ran on a machine without a usable window or sound device, which kept
  them CI-safe from day one.
- **Per-game TOML with strict keys** (`tools/recomp/config/`): unknown keys are errors with file and line.
  Entry points, RAM code regions, skips, byte patches, C replacement functions, and the save type all
  live there.
- **Trace feedback.** The runtime's interpreter logs each address it had to run; the recompiler takes the
  log back as entry points. On the 34 tonc programs built from source, static discovery finds 84% of
  the ROM functions that actually run and one feedback pass finds 100%; every compat title runs with no
  interpreter fallback after one pass. For AnySwitch this is the main safety net: plan for it from the
  start rather than chasing 100% static discovery.
- **Compatibility tracker** (`compat/*.toml`, `compat/schema.md`, `tools/compat-site/generate.py`,
  `pages.yml`): platform-neutral stages and evidence rules, frame-hash checkpoints checked in CI, a static
  page and badges. Only `kind`, `rom` and `rom_id` are GBA-specific.
- **Reference-oracle pattern.** Pictures, audio and BIOS results were checked against mGBA through its
  libretro core driven by a Python script, with the references committed and the oracle rebuilt on
  demand. AnySwitch needs the same with a Switch emulator as the oracle.

## Change on the way

- **Native frames and the interpreter.** Generated functions call each other natively, and the
  interpreter fills the gaps. Two bugs hid here until the compat titles ran for a full minute: the
  interpreter ran on past the return address of a pending native call (the native caller was then
  abandoned on the host stack, which overflowed after a few thousand frames in tonc `bigmap` and
  `m7_ex`), and a function whose code extends below its entry started at its lowest address. Both are
  fixed, and nested resumes are bounded as a safeguard. AnySwitch's threads and coroutines will stress
  the same seams more: design the dispatcher around explicit continuation points, and test with long
  runs that mix native and interpreted code, not only with per-instruction differential tests.
- **Static discovery limits.** Function pointers built at run time and calls from RAM-copied code are
  invisible to static analysis. Feedback covers them, but it means the first run of every title is partly
  interpreted and slow (tonc `oacombo` ran at 15 frames/s before feedback, 540 after).
- **Checkpoints are regression guards.** The recorded frame hashes come from this runtime; they catch
  changes, not errors. Correctness evidence comes from oracle comparisons, recorded per title in
  `evidence`.

## Does not transfer

- The GBA hardware model (PPU, APU, DMA, timers, cartridge saves) and the HLE BIOS.
- The ARMv4T decoder and C emitter; AnySwitch relinks AArch64 instead.
- ROM embedding by appending to the executable: Switch titles are far too large, and AnySwitch reads
  the user's dump at run time.
