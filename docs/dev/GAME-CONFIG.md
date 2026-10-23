# Per-game config and trace feedback

`anygba-recomp game.gba --config game.toml` reads one TOML file. Every section is optional. Unknown keys
are errors, so a typo cannot silently disable a setting. Addresses are integers; TOML accepts `0x` hex.

```toml
[game]
title = "tonc irq_demo"

# Extra function entry points the static analysis cannot see.
[[entry]]
addr = 0x08000100
mode = "thumb"          # "arm" or "thumb"

# Code the game copies from ROM into RAM before running it there. The bytes at
# rom_src..rom_src+size are analysed as if they lived at ram_dst, and ram_dst becomes
# an entry point in `mode`. ram_dst must lie in EWRAM (0x02000000-0x0203FFFF) or
# IWRAM (0x03000000-0x03007FFF).
[[ram_region]]
rom_src = 0x08000000
ram_dst = 0x02000000
size = 0x8000
mode = "arm"

# Known data inside code areas: never decoded.
[[skip]]
start = 0x08001000
end = 0x08001200        # exclusive

# Bytes replaced in the analysed image (and in the ROM bytes backing a RAM region).
[[patch]]
addr = 0x08000010
bytes = "00 00 a0 e1"   # memory order; spaces and underscores are ignored
```

## Trace feedback

When generated code reaches an address with no recompiled function, the runtime interprets it and
appends one line to `anygba-miss.log`:

```
0x03000124 arm rom=0x08001234
0x0800abcd thumb
```

`rom=` names the ROM address whose bytes match the RAM code the runtime found, so the recompiler can
analyse that RAM code on the next pass. Pass the log back with `--miss-log anygba-miss.log` (repeatable).
Each address becomes an entry point; a RAM address that no region covers gets a region backed by the
reported ROM bytes, reaching to the end of that RAM area or of the ROM, whichever comes first. Lines
starting with `#` are comments; duplicates are ignored.

## How functions are found

Discovery starts at the ROM entry (0x08000000, ARM) plus the config and miss-log entries, then walks each
function with a worklist and per-path constant tracking. It adds:

- `BL` targets, including calls through `bx rN` veneers when the caller's `rN` is a known constant;
- `BX`, `MOV PC`, and `LDR PC` targets that are constants (literal pools, `ADR`, `add rX, pc, #n`), with
  the mode taken from bit 0 for `BX`;
- the value stored to the BIOS IRQ vector (0x03007FFC or its mirror 0x03FFFFFC), always ARM;
- jump tables: `ldr pc, [pc, rI, lsl #2]`, `add pc, pc, rI, lsl #2` with a branch list, and the GCC Thumb
  `lsl / ldr =table / add / ldr / mov pc` idiom, bounded by the preceding `cmp rI, #n`. Without a bound,
  the table is read as function pointers for as long as its entries look like code in the same area;
- literal-pool values that look like code pointers, only if they decode cleanly and start like compiled
  code (unconditional in ARM; in Thumb they must also return and open with `push {.., lr}` unless short).

A branch to another function's entry is a tail call. Calls to functions that never return (found by a
fixpoint over returns and tail calls) end the path, so literal pools after a `bl panic` are not decoded.
`anygba-recomp game.gba --list` prints every function with its disassembly and the analysis notes.
