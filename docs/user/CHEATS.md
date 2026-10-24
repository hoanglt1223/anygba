# Cheats

Games built with AnyGBA accept GameShark, Action Replay, CodeBreaker and raw codes, like
VisualBoyAdvance. There are three ways to use them:

- **The menu.** Press Escape (gamepad: Back+Start) and choose *Cheats*. Cheats from a cheat database
  appear in categories; *My codes* holds codes you typed. Your choices are kept for the next time you
  play.
- **A cheat file.** `--cheats FILE` turns on the cheats in a file for one run (format below).
- **The command line.** `--cheat [FORMAT:]CODE` turns on one cheat for one run, for example
  `--cheat "cb:82025E30 0063"` or `--cheat "arv3:D8BAE4D9 4864DCE5;A86CDBA5 19BA49B3"`. Separate lines
  with `;` or `,`.

Cheats that change the game's code (ROM patches) work too; the patched part of the game then runs a bit
slower. Cheats are separate from save states: loading a state keeps your cheats on or off as they are.

## In the menu

| Button | Cheats page | Value picker |
|---|---|---|
| Up, Down | move | previous, next choice |
| L, R | page up, down | 10 back, forward |
| A | open a category, turn a cheat on or off (a cheat with a value opens the picker) | turn on with this value |
| Right | change the value of a cheat | next |
| Select | delete one of your codes | turn the cheat off |
| B | back | back without changing |

In a list of names, typing a letter jumps to the next name that starts with it.

**Typing a code:** *My codes* > *Add code...*. Type the hex digits on the keyboard (AnyGBA adds the space
after the first 8 digits), press Enter after each line, and Enter on an empty line to finish. Left and
Right choose the format; *Auto-detect* works for almost every code. Escape cancels. A code that cannot be
decoded is not added, and the message says why.

## Formats

| Name | Device | Code lines |
|---|---|---|
| `auto` | decided from the code | any of the below |
| `gsv1` | GameShark, Action Replay v1/v2 (encrypted, as printed) | `XXXXXXXX YYYYYYYY` |
| `gsv1raw` | the same, already decrypted | `XXXXXXXX YYYYYYYY` |
| `arv3` | Action Replay v3, GameShark SP (encrypted, as printed) | `XXXXXXXX YYYYYYYY` |
| `arv3raw` | the same, already decrypted | `XXXXXXXX YYYYYYYY` |
| `cb` | CodeBreaker, Xploder (plain, or encrypted after a `9...` code) | `XXXXXXXX YYYY` |
| `raw` | address and value, VisualBoyAdvance style; a cartridge address (`08xxxxxx`) patches the ROM | `AAAAAAAA:VV`, `:VVVV` or `:VVVVVVVV` |

Supported: RAM and I/O writes (8, 16, 32 bits), fills and increment slides, pointer writes, add/or/and,
byte lists, all conditions (equal, not equal, less, greater, AND; skip the next code, the next two, or an
if/else/endif block), the joypad condition (CodeBreaker `D0000020`), codes that need the device's button
(the *CheatButton* hotkey, F12 by default), and ROM patches. Master/enable codes (game ID, hook) are
accepted and do nothing, because AnyGBA runs every cheat once per frame anyway.

Not supported: `DEADFACE` codes, which change the encryption with tables that are not published. They
are refused with a message.

**AnyGBA raw extensions** for games that move or encrypt their data (see `cheats/BPEE.toml` for Pokemon
Emerald):

- `[PPPPPPPP]+OFFSET:VALUE` writes at the address stored at `PPPPPPPP`, plus `OFFSET`.
- `VALUE^TARGET` (TARGET written like an address or a pointer) XORs the value with what is stored there
  first.

## Cheat databases

A cheat database lists ready-made cheats for one game, in categories, and can let you pick a value from
a list (an item, a Pokemon) or a range (an amount). A game looks for databases named after the ROM's
game code (`BPEE.toml`) or CRC-32 (`1f1c08fb.toml`, also `1f1c08fb-2.toml` and so on) in:

1. `cheats/` next to the program (`anygba build` puts the databases that ship with AnyGBA there),
2. `cheats/` in the per-user data folder (`%APPDATA%\anyport\anygba\cheats\` on Windows),
3. files given with `--cheat-db FILE`.

AnyGBA ships a curated database for Pokemon Emerald. For many more games, convert the community lists of
[libretro-database](https://github.com/libretro/libretro-database) (CC BY-SA 4.0, so not included):

```sh
python tools/cheats/import-libretro.py --out "%APPDATA%\anyport\anygba\cheats"
```

Community lists contain typos; entries that cannot be decoded are skipped with a warning when the game
starts.

### Database format

```toml
[game]
title = "Pokemon - Emerald Version (USA, Europe)"
code = "BPEE"
crc32 = ["1F1C08FB"]           # optional: the database is ignored for other ROM images

[lists]
items = ["1 Master Ball", "68 Rare Candy", "0x6E Nugget"]   # value, a space, the name

[[cheat]]
name = "Items/Rare Candy x99"   # categories separated by '/'
format = "raw"                  # optional, default auto
code = ["[03005D8C]+0564:0044", "[03005D8C]+0566:0063^[03005D90]+00AC"]
note = "Items pocket, slot 2"   # optional

[[cheat]]
name = "Items/Item of your choice x99"
format = "raw"
code = ["[03005D8C]+0560:{v:4}", "[03005D8C]+0562:0063^[03005D90]+00AC"]
param = { label = "Item", list = "items" }    # or { label = "Amount", min = 0, max = 999999 }
default = 68
```

`{v:N}` in a code is replaced by the chosen value in N hex digits.

## The per-game cheat file

What you turn on in the menu is saved next to the save file as `<title>-<hash>.cheats.toml`. You can
edit it while the game is not running, and `--cheats FILE` reads the same format:

```toml
[[cheat]]                      # your own codes
name = "Code 1"
format = "cb"
code = ["82025E30 0063"]
enabled = true

[database]                     # cheats from a database: true, or the chosen value
"Items/Master Ball x99" = true
"Items/Item of your choice x99" = 68
```
