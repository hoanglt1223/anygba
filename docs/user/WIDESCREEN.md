# Widescreen

`widescreen=16:10` adds 8 columns on each side of the GBA's 240 (256x160), `widescreen=16:9` adds 22
(284x160). The extra columns are drawn by the HD renderer ([HD-PACKS.md](HD-PACKS.md)) from what the
game already has in VRAM: background tile maps beyond the screen edge, sprites positioned off screen,
rotated layers continued. The middle 240x160 is always exactly the normal picture.

```sh
<Title> --video "widescreen=16:9"
<Title> --video "widescreen=16:9;internal-scale=4"
```

It is also in the menu's Video page and the game TOML's `[video]` table, and changes at once.

Games only keep a little more than the screen in memory, and many put text boxes or the HUD on a
layer that would otherwise stretch, so each game needs rules. Without rules every layer is widened
from its tile map, which shows repeated or stale map parts in most games.

## Per-game rules

`widescreen/<game code>.toml`, next to the game program (`anygba build` copies the ones shipped in
the repository's `widescreen/` folder) or in the per-user data folder. `<game code>` is the 4-letter
code in the ROM header.

```toml
title = "Pokemon - Emerald Version (USA, Europe)"

[default]                  # used when no scene below matches
mode = "pillarbox"         # "widen" or "pillarbox" (black bars)

[[scene]]                  # the first scene whose conditions all hold is used, frame by frame
name = "overworld"
when = ["u32 0x030022C4 == 0x08085E5D"]   # size address operator value
mode = "widen"
hud = ["bg0"]              # layers (or "obj") kept inside the middle 240 columns
clamp = []                 # layers whose edge column is repeated outwards
mirror = []                # layers reflected at the screen edge
provider = "pokeemerald-field"            # optional native code, see below
[scene.settings]           # numbers for the provider
map_header = 0x02037318

[[patch]]                  # ROM halfword patches applied while widescreen is on
address = 0x08000000
value = 0x0000
```

- Conditions read memory at the end of each frame: `u8`, `u16` or `u32`, an address, one of
  `== != < <= > >=` or `&` (any of the bits set), and a value. Typical choices: the game's main
  callback pointer, a "battle in progress" flag, a map number.
- Layers not named keep `Map`: the extra columns come from the layer's own tile map, bitmap or sprites.
- Patches use the cheat engine's ROM patching (the patched code runs in the interpreter), e.g. to
  widen a game's sprite culling. Cheats on the same address win.
- `ANYGBA_WIDESCREEN_TRACE=1` prints every scene change with its frame number, for writing rules.

## Providers

A provider rebuilds tile map entries that the game does not keep. Before widening, it predicts the
visible part of the screen and requires it to match what the game drew (97% of the drawn tiles);
if it does not, that frame is pillarboxed instead of showing wrong tiles.

| Provider | Games | Settings |
|---|---|---|
| `pokeemerald-field` | Pokemon Emerald, and Ruby/Sapphire/FireRed-style engines with the same layout (pret decompilations) | `map_header` (gMapHeader), `backup_layout` (gBackupMapLayout), `save_block1_ptr` (gSaveBlock1Ptr) |

`pokeemerald-field` reads the map grid around the camera (with its connections and border blocks)
and the tilesets' metatile definitions, and draws the three map layers (BG1-BG3) for any position, as
the game's own map drawing would.

## Status

| Game | Code | Status |
|---|---|---|
| Pokemon Emerald (USA, Europe) | BPEE | Overworld widened (16:9 and 16:10) with the map rebuilt beyond the game's buffer; text boxes and menus stay in the middle. Battles widened: background and sprites from VRAM, the text box and battle menus with their frame colour repeated outwards. Every other scene (title, intro, transitions into battle, bag, party, Pokedex) is pillarboxed. The game hides people and objects once they are off its own screen, so they can appear late near the 16:9 edges: its culling (UpdateObjectEventOffscreen) hides an object once it is more than 16 pixels past the screen edge, while 16:9 shows 22. A ROM patch widening it to 32 pixels applies, but headless walks in Littleroot showed no frame where it changed the picture, so it is not shipped. |

Per-game results are also recorded in `compat/<title>.toml` (`widescreen`, `widescreen_notes`).
