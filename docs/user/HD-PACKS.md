# HD rendering, HD packs and AI upscaling

AnyGBA's picture unit knows which tile, sprite and palette is drawn where, so it can do more than
scale the finished 240x160 picture ([VIDEO.md](VIDEO.md)):

- **Internal resolution**: compose the layers at N x N sub-pixels per GBA pixel, so rotated and
  scaled backgrounds and sprites (Mode 7 floors, spinning sprites) are drawn without stair steps.
- **Per-layer filters**: run a pixel-art scaler on each layer before the layers are combined, so
  sprites keep sharp outlines against the background.
- **HD packs**: replace tiles, sprites and bitmap screens with your own art at any whole-number
  scale up to 16x, with live reload while the game runs.
- **AI upscaling**: generate a whole pack from a dump with an upscaling model, or upscale the
  picture in real time on the GPU.

Everything here is off by default and changes at once from the menu's Video page, the command line
(`--video`) or the game TOML's `[video]` table. Widescreen is described in [WIDESCREEN.md](WIDESCREEN.md).

## Settings

| Key | Values | Default |
|---|---|---|
| `internal-scale` | 1 to 8: GBA pixels are drawn as N x N sub-pixels | 1 |
| `layer-filter` | any `--filter` scaler (`xbrz4x`, `hq3x`, ...), run on each layer before compositing | `nearest` |
| `smooth-affine` | `true`/`false`: rotated/scaled layers and sprites sampled per sub-pixel | true |
| `hd-pack` | `true`/`false`: use the game's pack when one is found | true |
| `hd-dump` | `true`/`false`: write every new piece of art to the dump folder | false |
| `ai` | `off`, `frame` (upscale the finished picture), `layers` (upscale each layer) | `off` |
| `ai-model` | `general-x4v3` (small, real time), `x4plus` (larger, sharper on big shapes) | `general-x4v3` |

```sh
<Title> --video "internal-scale=4;layer-filter=xbrz4x"
<Title> --hd-pack D:/packs/BPEE --video internal-scale=4
<Title> --video ai=frame
```

Command-line options of the game program:

- `--hd-pack DIR`: use the pack in DIR. Without it the game looks for `hdpacks/<game code>/` next to
  the program, then in the per-user data folder (`<game code>` is the 4-letter code in the ROM
  header, e.g. `BPEE`).
- `--hd-dump DIR`: dump to DIR (default `<data folder>/hdpacks-dump/<game code>/`).
- `--hd-dump-filter F`: what to dump, comma-separated: `all`, `tiles`, `objs`, `bitmaps`, `bg0`-`bg3`,
  `blank` (also fully transparent art) and `frames=N` (default 3, see below).
- `--ai-device D`: `auto` (DirectML, then CUDA, then CPU), `dml`, `cuda` or `cpu`.
- `--screenshot N:FILE` and `--screenshot-size WxH`: save frame N as a PNG as the window would show
  it, also with `--headless`.

With `internal-scale` above 1 the whole-picture scaler of `--filter` is not used (the picture is
already at a higher resolution); screen effects, colour and gamma still apply. Debanding only works on
1x pictures.

## How the HD picture is made

Each visible line's registers, palette, OAM and affine reference points are copied as the line is
drawn, and VRAM is copied whenever the game wrote to it during the frame, so HBlank effects and
mid-frame changes look the same as at 1x. At the end of the frame:

1. Every layer is drawn at 1x exactly as the scanline PPU draws it, and each tile and sprite is
   identified by its content key (below) and looked up in the pack.
2. Each line is rebuilt at N x N sub-pixels: replacement art is sampled per sub-pixel (flips and
   mosaic included); rotated/scaled layers and sprites are sampled at sub-pixel positions; other
   pixels come from the 1x layer, scaled by `layer-filter` if set.
3. Layers are composited per sub-pixel with the line's windows, priorities, blending, brightness
   effects and the OBJ window.

Sub-pixel (0, 0) of every pixel equals the 1x picture, so without a pack the HD picture shrunk back
to 1x is identical to the normal one (the test suite checks this on 14 demos and at 2x-4x).

Limits: replacement art and filters do not change the game's logic, so collision, text placement and
animation timing stay at GBA resolution. Window edges stay on GBA pixel boundaries.

## Pack format

A pack is a folder of PNG files named by key, in any subfolders, plus an optional `pack.toml`:

```
hdpacks/BPEE/
  pack.toml
  tiles/t-3f2a9c0e11d4b7a2.png      # one 8x8 tile, as shown (its colours)
  tiles/ti-77c0a1b2c3d4e5f6.png     # one 8x8 tile in every palette (its colour indices)
  objs/o-0a1b2c3d4e5f6a7b.png       # a whole sprite, e.g. 32x32
  objs/oi-....png                   # a whole sprite in every palette
  bitmaps/b-....png                 # a whole mode 3/4/5 screen
```

- Images may be 1x to 16x the original size (an 8x8 tile as 8x8 ... 128x128, a 32x16 sprite as
  64x32, ...); width and height must use the same factor. Other sizes are ignored with a message.
- Alpha below 128 is transparent: replacement art can be transparent where the original was opaque
  and the other way round.
- Art is resampled to the internal scale: box-filtered down, and up by nearest neighbour (or
  bilinear with `upscale = "linear"` in pack.toml). Art at exactly the internal scale is used as is.
- Lookup order: a sprite's own art (`o-`, then `oi-`), else its 8x8 tiles; a tile's `t-` art, then
  `ti-`. Anything without art is drawn from the original (with `layer-filter` if set).
- The pack is reloaded within a second when files are added, changed or removed (hot reload), so you
  can edit art while the game runs.

`pack.toml` (optional):

```toml
title = "Emerald HD"
upscale = "nearest"   # or "linear": how art smaller than the internal scale is enlarged
```

### Keys

A key is the 64-bit FNV-1a hash (offset 0xcbf29ce484222325, prime 0x100000001b3) of these bytes,
written as 16 lower-case hex digits after the prefix:

| Prefix | Bytes hashed |
|---|---|
| `t` | `'T'`, width and height as 16-bit little-endian (8, 8), then the 64 pixels row by row as 16-bit little-endian BGR555, 0x8000 for transparent (colour 0) |
| `ti` | `'I'`, width, height, then the byte 4 or 8 (bits per pixel), then the 64 colour indices |
| `o` | `'O'`, width, height, then the sprite's pixels as for `t` |
| `oi` | `'P'`, width, height, bits per pixel, then the sprite's colour indices |
| `b` | `'B'`, width, height (240x160 or 160x128), then the pixels as for `t` |

Pixels are in the art's own orientation (before horizontal/vertical flips), so flipped copies share
one image.

## Dump mode

With `hd-dump` on (menu, `--video hd-dump=true` or `--hd-dump DIR`), every new tile (text and
affine backgrounds), whole sprite and bitmap screen is written once as a 1x PNG with its key as the
name, and `index.json` lists them:

```json
{"format": 1, "game": "BPEE", "title": "POKEMON EMER", "entries": [
  {"key": "t-000268edd0569fff", "kind": "tile", "file": "tiles/t-000268edd0569fff.png",
   "indices": "ti-542ed57b1f7a21be", "width": 8, "height": 8, "layer": "bg3",
   "first_frame": 3090, "last_frame": 4227, "frames": 6}]}
```

`frames` counts the frames the art appeared in. Tiles also get `<key>.context.png`: the tile with its
neighbours on the layer when first seen (24x24, the tile at 8,8), used by AI upscaling for seamless
edges. A dump continues across runs. A dump folder is itself a valid pack (each image replaces itself).

Screen fades and palette animations change colours every frame, and every step would be new art.
Art is therefore only written once it has been seen in `frames=N` frames (default 3); `anygba hdpack
dedupe` finds what remains (art that differs only by palette) and can fold it into palette-independent
images.

## Pack creator: `anygba hdpack`

```sh
anygba hdpack dedupe DUMP [--apply]
anygba hdpack preview DIR -o sheet.png [--pack PACK] [--scale N] [--kind tile|obj|bitmap] [--limit N]
anygba hdpack import SRC PACK [--dump DUMP] [--title T]
anygba hdpack ai-upscale DUMP PACK [--model general-x4v3|x4plus] [--device auto|dml|cuda|cpu]
                                   [--kinds tiles,objs,bitmaps] [--min-frames N] [--no-context] [--force]
anygba hdpack fetch-runtime [--directml]
```

- `dedupe` groups art by colour indices. With `--apply` it keeps the most used variant of each group
  as `shared/<ti-|oi-key>.png` (one image for every palette) and moves the variants to `variants/`.
- `preview` draws a contact sheet, most used first; with `--pack`, each original beside its
  replacement.
- `import` checks key-named PNGs (sizes against the dump's index with `--dump`) and copies them into
  the pack's folders, writing `pack.toml` if missing.
- `ai-upscale` writes a 4x pack from a dump; tiles are upscaled with their context. `--min-frames`
  skips art seen in few frames (fade steps). On the development PC (Radeon RX 6600 XT, DirectML) the
  `x4plus` model upscaled 2,302 Pokemon Emerald images in 147 s.

## AI upscaling

Models are run with [ONNX Runtime](https://onnxruntime.ai) (MIT), loaded when AI upscaling is
first used, so games do not depend on it. It is looked up in:

1. `ANYGBA_ONNXRUNTIME`: the library file or its folder;
2. next to the game program (`onnxruntime.dll`, `libonnxruntime.so`, `libonnxruntime.dylib`);
3. the AI cache (`ANYGBA_AI_CACHE`, default `<data folder>/ai`). On Windows x64 the pinned ONNX
   Runtime 1.24.4 DirectML build is downloaded there on first use (12 MB, SHA-256 checked). It uses
   Windows' own DirectML.dll; `anygba hdpack fetch-runtime --directml` also fetches Microsoft's
   DirectML redistributable for Windows versions whose DirectML is too old. On Linux and macOS
   install ONNX Runtime 1.24 or later and set `ANYGBA_ONNXRUNTIME`.

Models are downloaded on first use into the cache with `curl` and unpacked with `tar` (or `unzip`),
checked against pinned SHA-256 hashes, and never shipped with AnyGBA. They are published with fixed
128x128 inputs; AnyGBA stores a copy whose inputs take any size, so a whole frame is one run.

Downloads in the player run on a worker thread, so the game keeps running: the on-screen display
shows `AI download: <what>: <MB so far>` about once a second. Turning AI off again (the video menu
or settings) cancels the download; a cancelled or failed download leaves no partial file and the
game stays on the picture without AI, with a message saying so. `ANYGBA_AI_MIRROR=<base URL>`
fetches each archive from `<base URL>/<archive file name>` instead (an offline mirror; the pinned
hashes still apply).

| Model | Weights | Licence | ONNX export | Archive SHA-256 |
|---|---|---|---|---|
| `general-x4v3` | realesr-general-x4v3, [xinntao/Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) | BSD-3-Clause | Qualcomm AI Hub (qai-hub-models 0.65.0) | `fc094329b24a00c9...` |
| `x4plus` | RealESRGAN_x4plus, xinntao/Real-ESRGAN | BSD-3-Clause | Qualcomm AI Hub (qai-hub-models 0.65.0) | `05aa31323c6a7e83...` |

Full URLs and hashes are in `runtime/src/video/hd/ai.cpp`.

- **`ai=frame`**: the finished 1x picture (widescreen included) is upscaled 4x on a worker thread and
  shown when ready, one frame behind the game. Measured on the development PC, general-x4v3 on
  DirectML: 14 ms per 240x160 frame (960x640 out), so 60 fps with headroom; the window then scales
  960x640 to 1080p on the GPU.
- **`ai=layers`**: each layer is upscaled before compositing (internal scale 4). That is five model
  runs per frame: far from real time, useful for screenshots and for judging a model.
- Transparent pixels are filled from their neighbours before the model runs, and transparency is
  scaled separately, so sprites keep clean edges.
- `x4plus` removes fine noise-like texture (grass dots, dithering) more than `general-x4v3`; try both.
