# Per-game config and trace feedback

`anygba-recomp game.gba --config game.toml` reads one TOML file. Every section is optional. Unknown keys
are errors, so a typo cannot silently disable a setting. Addresses are integers; TOML accepts `0x` hex.

```toml
[game]
title = "tonc irq_demo"
save = "flash128"       # none, sram, flash64, flash128, eeprom, eeprom512, eeprom8k (default: detect)
rtc = true              # cartridge real-time clock (default: on for the games that shipped with one)
sources = ["hacks.c"]   # C or C++ files compiled into the game, relative to this file

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

# The function at addr runs a hand-written C function from `sources` instead of its
# translation. The address becomes an entry point.
[[replace]]
addr = 0x08000400
mode = "thumb"
function = "skip_intro"

# The function at addr (or the [functions] entry `symbol`) runs the C++ override registered as
# `impl` while the game runs with --overrides on (docs/dev/OVERRIDES.md).
[[override]]
symbol = "StringCopy"
impl = "pokemon_gen3_StringCopy"
enabled = true          # optional; false: compiled in, used only with --overrides all
engine = "pokemon-gen3" # optional tag
verified = true         # optional; set by `anygba difftest --mark-verified` after a pass
games = ["BPEE"]        # optional; only for these game codes (ROM header 0xAC)

# Named guest functions, for [[override]] symbol = "...". Symbols files may hold the same table.
[functions]
StringCopy = { addr = 0x08008BA0, mode = "thumb" }

# Default video settings of the generated game (keys and values: docs/user/VIDEO.md).
# The player's menu choices and command-line options override them.
[video]
filter = "xbrz4x+lcd-grid"
color = "gba-lcd"
gamma = 1.2
deband = true
output = "1080p"
fullscreen = false
integer-scale = true
```

## Replacement functions

A replacement has the generated functions' signature, `void name(anygba_cpu* c)`, and must leave the CPU
the way the original would: results in `c->r[...]`, and `c->r[15]` set to where execution continues.
`ag_return(c)` does what `bx lr` does. Memory goes through the same helpers generated code uses
(`ag_ldr`, `ag_wr16`, ... in `anygba/cpu.h`):

```c
#include <anygba/cpu.h>

/* The intro's frame counter: return 0 so the title screen comes up at once. */
void skip_intro(anygba_cpu* c) {
    c->r[0] = 0;
    ag_return(c);
}
```

Every call reaches the replacement, including direct calls from other generated functions: the original
`f_XXXXXXXX` becomes a one-line call to it. Function names may not start with `f_`, `ag_` or `anygba_`.
`[[replace]]` without `sources` is an error.

## Overrides

`[[override]]` is the richer form for C++ rewrites: named targets through `[functions]` tables, a guest
API instead of the raw CPU, modules shared between games (`anygba build --overrides DIR`), startup checks
and `--overrides off` for comparison. Schema, guest API and rules: [OVERRIDES.md](OVERRIDES.md).

## Symbols: named memory for agents

A `[symbols]` table names game variables for AI agents ([../user/AI-CONTROL.md](../user/AI-CONTROL.md)):
`read_named` reads `money` or `party[0].hp`, and the play journal snapshots the ones marked `journal`.
The recompiler ignores it; `anygba build --config` copies the file next to the game as
`symbols/<game code>.toml`, where the game reads only this table. Shipped files live in
`compat/symbols/` (installed to `share/anygba/symbols/`); games also read `--symbols FILE`. The game checks
the table when it starts and stops with the file, line and problem.

```toml
[symbols.save_key]            # the name agents use: letters, digits, '_' (no '.', '[', ']', spaces)
pointer = 0x03005D90          # optional: address of a 32-bit pointer; `address` is then an offset from it
address = 0xAC
type = "u32"                  # u8 u16 u32 s8 s16 s32 bool bytes text struct

[symbols.money]
pointer = 0x03005D8C
address = 0x490
type = "u32"
xor = "save_key"              # optional: XORed with another integer symbol (low bytes for u8/u16)
journal = true                # optional: in the journal's periodic snapshots
description = "shown by list_symbols"

[symbols.party]
address = 0x020244EC
type = "struct"
size = 100                    # bytes per element; required for struct, bytes and text
count = 6                     # optional: an array (read as party, party[2], party[2].hp)
fields.nickname = { offset = 0x08, type = "text", size = 10, charset = "pokemon" }   # charset: ascii, pokemon
fields.hp = { offset = 0x56, type = "u16" }
fields.moves = { offset = 0x0C, type = "u16", count = 4 }                            # party[0].moves[1]
fields.badge = { offset = 0x10, type = "u8", bit = 7 }                               # one bit, as a boolean
```

Fields take `offset`, `type` (not `struct`), `size`, `count`, `bit`, `xor` and `charset`, and must fit in
the struct's `size`. One read returns at most 64 KB. `bytes` reads as hex; `text` stops at the charset's
terminator (0 for ASCII, 0xFF for the Gen 3 Pokemon charset).

## Save type

Without `save`, the runtime picks the type from the library ID string in the ROM (`SRAM_V`, `FLASH_V`,
`FLASH512_V`, `FLASH1M_V`, `EEPROM_V`). Set it when a ROM has no ID or the wrong one. Plain `eeprom`
decides between 512 bytes and 8 KB from the length of the first EEPROM DMA transfer; set `eeprom512` or
`eeprom8k` for games that access the EEPROM without DMA. A generated game's `--save-type` overrides both.

## Real-time clock

The S-3511 clock on the cartridge GPIO port (0x080000C4-0x080000C9) is on by default for the games that
shipped with it: Pokemon Ruby, Sapphire and Emerald, Boktai 1-3, Rockman EXE 4.5 and Sennen Kazoku
(by game code, `runtime/src/system.cpp`). `rtc = true` or `false` overrides that list, and a generated
game's `--rtc on|off` overrides both. The clock reads the host's local time; date and time writes from
the game are ignored.

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

Games put different code at the same RAM address (overlays, routines built on the stack), so a
compiled RAM function only runs while RAM still holds the bytes it was compiled from: the dispatch table
records their ROM source (`code_src`), and both the dispatcher and the function's `AG_ENTER_RAM` compare
them on every entry. Anything else at that address is interpreted.

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
