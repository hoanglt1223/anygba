# Cheats

Code: `runtime/src/cheats/` (`codes` decodes and decrypts, `engine` runs, `cheat_list` holds databases
and the user's codes), `runtime/src/rom_patch.cpp`, `AG_ENTER` in `cpu.h`, `code_footprint` in the
recompiler. User documentation: [../user/CHEATS.md](../user/CHEATS.md).

## Decoding

Every device format is decoded into one small instruction list (`cheats::Op`): writes with fill counts
and slides, writes through pointers, add/or/and, byte lists, conditions that skip N codes or open an
if/else/endif block, "stop", and ROM patches. The engine runs each enabled cheat's list once per frame
from the player's VBlank hook, with the bus's normal read/write functions, as a cheat device's hook
routine does.

| Format | Source of the description |
|---|---|
| GameShark / Action Replay v1, v2 | GBATEK "GBA Cheat Codes - Gameshark/Action Replay V1/V2" |
| Pro Action Replay v3 / GameShark SP | GBATEK "GBA Cheat Codes - Pro Action Replay V3" |
| CodeBreaker / Xploder | GBATEK "GBA Cheat Codes - Codebreaker/Xploder" |
| Raw | VisualBoyAdvance-style `AAAAAAAA:VALUE`, plus AnyGBA's pointer and XOR forms |

The encryption is reimplemented from those descriptions (TEA with the published seeds for GameShark and
Action Replay; the bit permutation and seed generator for CodeBreaker's `9` code). No emulator code was
used: AnyGBA is MIT and VisualBoyAdvance is GPL. The TEA implementation is checked against published
master codes (Emerald's Action Replay v3 master decrypts to `45455042 001DC0DE`, "BPEE"; Mario & Luigi's
GameShark master to `45383841 001DC0DE`). No published CodeBreaker plain/encrypted pair was found, so its
cipher is only checked for determinism; treat encrypted CodeBreaker codes as unverified.

`DEADFACE` codes change the GameShark/Action Replay seeds through two tables stored in the device; GBATEK
does not list them, so these codes are refused with a message instead of producing wrong writes.

**Auto-detection.** Codes from lists do not say which device they are for. A line with `:` or a pointer
is raw; 12-digit lines are CodeBreaker; 16-digit lines are decoded as encrypted Action Replay v3,
encrypted GameShark v1/v2, plain Action Replay v3 and plain GameShark in that order, and the first
decoding whose every access lands in RAM, I/O or video memory (ROM patches in a cartridge) wins. Garbage
from decrypting a plain code, or not decrypting an encrypted one, almost never passes. On the libretro
database 96% of the codes decode (`ANYGBA_CHEAT_DB_DIR`, see the imported-database test); the rest are
typos in the lists.

**Raw extensions.** Some games move their save data and encrypt values with a key chosen per saved game
(Pokemon Emerald: money, coins and bag quantities). Device codes cannot follow either, which is why the
community lists for such games need "anti-DMA" ROM patches and still cannot set money. AnyGBA's raw format
can: `[PPPPPPPP]+OFFSET:VALUE` writes at the 32-bit pointer stored at `PPPPPPPP` plus `OFFSET` (and skips
the write while the pointer is 0), and `VALUE^TARGET` XORs the value with the same number of bytes at
`TARGET` before writing.

## ROM patches and recompiled code

Device ROM patches make the cartridge return another halfword at an address. On real hardware that
changes code and data alike. In AnyGBA data reads see the change directly (`System::set_rom_patches`
writes the patched halfwords into the ROM image and restores the originals when the cheat is turned off),
but compiled functions have the original instructions built in, and PC-relative literal loads were folded
into constants by the recompiler.

So patched code runs in the interpreter:

- The recompiler records each function's *code footprint*: the range of ROM bytes its compiled body
  depends on (its instructions, the literal words it folded, its jump tables) in the function table
  (`anygba_function_entry::code_begin/code_end`).
- When the set of patches changes, the runtime marks every function whose footprint contains a patched
  address as bypassed. `lookup()` stops returning bypassed functions, so dispatch, indirect calls, and
  interrupt handlers go to the interpreter.
- Direct calls between compiled functions do not go through `lookup()`. Every generated ROM function
  therefore starts with `AG_ENTER(c, entry)`: while any patch is active (`anygba_cpu::rom_patched`, one
  load and branch per call otherwise), it asks the runtime whether this function is bypassed and if so
  returns with `r[15]` = its own entry. The caller's `AG_CALL` sees an unexpected `r[15]`, calls
  `anygba_resume`, and the dispatcher interprets the patched code until it returns.

Limits: a function already running when the patch is turned on finishes its current call as compiled
(main loops that never return keep their old code until the game leaves them); RAM code copied from ROM
before the patch keeps the old bytes, as on hardware. The footprint is one range, so a function with a
far literal pool may be bypassed for a patch that does not touch it; that only costs speed.
`tests/player-cheats.cmake` patches the literal holding a pixel address in tonc's `first.gba` and checks
that the pixel moves.

## Databases and the cheat file

A cheat database (TOML, [../user/CHEATS.md](../user/CHEATS.md)) names cheats by category path and may give
an entry a value: the code holds `{v:N}` and the menu offers a list of named choices or a number range.
Games look for `<game code>.toml`, `<crc32>.toml` and numbered variants (`<crc32>-2.toml`) in `cheats/`
next to the program and in the per-user data folder; `anygba build` copies the matching files from
`<prefix>/share/anygba/cheats/` (this repository's `cheats/`). Entries that do not decode are skipped
with a warning so one typo does not hide a whole list; malformed TOML stops the program with the line.

What the user turns on, and their own codes, go to `<title>-<hash8>.cheats.toml` next to the save file,
rewritten on every change. Cheats are not part of save states: loading a state keeps the cheats as they
are, and turning a cheat on does not change older states.

`tools/cheats/import-libretro.py` converts libretro-database cheat lists (community data under CC BY-SA
4.0, so fetched by the user rather than shipped) into databases keyed by the CRC-32s of the matching
No-Intro entries, grouping cheats into categories by their names.
