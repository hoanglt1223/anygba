# Compatibility data

One TOML file per title in `compat/`. CI checks the checkpoints (`ANYGBA_COMPAT_TESTS=ON`) and
`tools/compat-site/generate.py` renders the files into `progress.html` and SVG badges. AnySwitch,
AnySWF and AnyPSP use the same format; only `kind`, `rom`, and `rom_id` are platform-specific.

```toml
title = "tonc: irq_demo"
kind = "homebrew"                    # homebrew | test-suite | commercial
source = "https://www.coranac.com/tonc/text/"
rom = "tonc/bin/irq_demo.gba"        # path in tests/corpus (homebrew and test suites only)
stage = "playable"                   # see below
evidence = "mGBA picture match; 60 s recompiled run, no interpreter fallback"
blockers = []                        # what keeps it from the next stage, one line each
misses = 0                           # interpreter fallbacks in the checkpoint run (before trace feedback)
checkpoint_frames = 3600
checkpoint_hash = "9f2c..."          # SHA-256 of the last frame, RGB555 little-endian (video only)
shards = 2                           # optional: funcs_NNN.c files when CI recompiles it
widescreen = "partial"               # optional: none | partial | full (docs/user/WIDESCREEN.md)
widescreen_notes = "overworld widened; battles pillarboxed"   # optional: which scenes, which rules
updated = 2026-10-24
```

`widescreen` says how far the game's widescreen rules (`widescreen/<rom_id>.toml`) go: `none` (no
rules, or every scene pillarboxed), `partial` (some scenes widened without visible errors, the rest
pillarboxed), `full` (every scene widened).

Commercial titles never name a ROM file or hash: they carry only `rom_id` (the 4-character game code from
the ROM header) and are checked by hand by whoever owns the game, so they have no `rom` or checkpoint.

Keep `rom`, `checkpoint_frames`, `checkpoint_hash` and `shards` on one line each with no trailing comment:
the CMake test glue reads them with a line regex.

## Stages

| Stage | Meaning |
|---|---|
| `fails` | Stops with an error before the first frame, or never shows a picture |
| `boots` | Shows pictures, but stops with an error or shows wrong output before its main content |
| `menus` | Title screen and menus work; the main content does not |
| `in-game` | Main content runs for the checkpoint length without errors; output not checked against a reference |
| `playable` | As `in-game`, and its pictures match a reference emulator (mGBA) where checked |
| `perfect` | No known difference from hardware: all of the title's own checks pass (test suites), or pictures and sound match the reference throughout |

A title only moves up a stage with the evidence recorded in `evidence`.

## Checkpoints

The checkpoint is a regression guard, not a correctness proof: the recorded hash is whatever the runtime
produced when the entry was last reviewed. When a runtime change alters a picture on purpose, re-record
the hash (the failing test prints the new one) and say why in the commit message. Audio is not hashed;
the resampler's floating point differs between compilers.
