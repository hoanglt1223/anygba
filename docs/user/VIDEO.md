# Video: filters, colour and resolution

A generated game shows the GBA's 240x160 picture through a filter chain:

```
colour -> ghosting -> scaler -> final scaling to the output -> screen effect
```

Settings come from four places, each overriding the one before:

1. the defaults (nearest, raw colour, window size);
2. the game's `[video]` table in its TOML config ([GAME-CONFIG.md](../dev/GAME-CONFIG.md));
3. what you last chose in the menu for this game, saved as `<game>.video` next to its save file;
4. `--filter` and `--video` on the command line.

## Filters

`--filter` takes one scaler, one screen effect, and `ghosting`, joined with `+` in any order:

```sh
<Title> --filter xbrz4x+lcd-grid
<Title> --filter hq3x+crt+ghosting
<Title> --filter sharp-bilinear
```

| Scaler | What it does |
|---|---|
| `nearest` | Sharp square pixels (default). Uneven pixel widths unless `integer-scale` is on. |
| `bilinear` | Smooth, blurry. |
| `sharp-bilinear` | Nearest to the largest whole multiple, then bilinear for the rest: sharp, with even pixels. |
| `scale2x`, `scale3x`, `scale4x` | EPX/AdvMAME: rounds diagonal steps, keeps the palette. |
| `eagle` | Fills corners where three neighbours agree. |
| `2xsai`, `super-2xsai`, `super-eagle` | Kreed's 2xSaI family: smooths diagonals with blended colours. |
| `hq2x`, `hq3x`, `hq4x` | hqx-style: compares neighbours in YUV and blends edges. |
| `xbr2x`, `xbr3x`, `xbr4x` | xBR: finds edge directions in a 5x5 window and cuts pixel corners along them. |
| `xbrz2x` ... `xbrz6x` | xBRZ-style: stricter edge rules, anti-aliased corners, up to 6x. |

| Screen effect | What it does |
|---|---|
| `lcd-grid` | Dark lines between the GBA's pixels. |
| `scanlines` | Dark gaps between lines. |
| `crt` | Strong scanlines plus an RGB aperture grille, brightened to compensate. |
| `ghosting` | Blends each frame with the one before, like the GBA's slow LCD. Games that flicker sprites for transparency rely on it. |

hqx, xBR and xBRZ are independent implementations from the published descriptions of those algorithms, not
ports of the reference code, so their output is close to but not identical with other emulators'
([THIRD_PARTY.md](../../THIRD_PARTY.md)).

## Other settings

`--video` takes `key=value` pairs separated by `;` (the same keys work in the game TOML):

| Key | Values | Default |
|---|---|---|
| `filter` | as `--filter` | `nearest` |
| `color` | `raw`, `gba-lcd` (darker, desaturated, like a real GBA screen) | `raw` |
| `gamma` | 0.5 to 2.5; above 1 brightens | 1 |
| `deband` | `true`/`false`: smooths gradients of one-level colour steps; hard edges stay | false |
| `output` | `window`, `720p`, `1080p`, `1440p`, `4k`, `desktop` | `window` |
| `fullscreen` | `true`/`false` | false |
| `integer-scale` | `true`/`false`: only whole multiples of 240x160 | false |
| `internal-scale`, `layer-filter`, `smooth-affine`, `hd-pack`, `hd-dump`, `ai`, `ai-model` | HD rendering, packs and AI upscaling: [HD-PACKS.md](HD-PACKS.md) | off |
| `widescreen` | `off`, `16:10`, `16:9`: [WIDESCREEN.md](WIDESCREEN.md) | `off` |
| `smoothing` | `true`/`false`: pictures between frames on high-refresh monitors (below) | false |

```sh
<Title> --video "color=gba-lcd;gamma=1.2;output=1080p;fullscreen=true"
```

`window` renders at the window's own size. The fixed outputs render at that resolution and scale the
result to the window or screen, keeping 3:2 with black bars; in a window they also resize the window
when it fits on the desktop. Output is always 32-bit colour.

## Smoothing

The game always runs at the GBA's 59.73 frames per second; a faster monitor cannot make it run
faster without speeding the game up. On a monitor of 120 Hz or more, `smoothing` fills the extra
refreshes with pictures between two GBA frames: the PPU draws the current frame again with the
background scroll, rotation reference points and sprite positions moved part of the way from the
previous frame. It does this only when the two frames differ by small movement alone (at most 16
pixels, no palette, register, sprite size or visibility changes, no bitmap modes); every other frame
is shown as it is, so smoothing never shows pixels the game would not draw. It is off with the HD
renderer, fast-forward and speeds other than 100%, and on monitors below 120 Hz.

Smoothing adds up to one GBA frame (about 17 ms) of input latency, because the pictures between
two frames can only be drawn once the second one exists. Turn it on in the Video page or with
Ctrl+I (the `Smoothing` hotkey).

## In the menu

Open the menu (Escape by default) and choose **Video**. Left/right changes a setting and it applies at
once; B goes back. Choices are saved for this game and used on its next start (command-line options
still win).

## How it runs

The colour conversion, ghosting and pixel-art scalers run on the CPU at no more than 6x the GBA
resolution (at most 1440x960), spread over all cores. The final scaling to the output and the screen
effect run on the GPU through SDL's renderer (Direct3D, Metal, OpenGL or Vulkan): the effect is a
brightness mask uploaded once per output size and multiplied over the picture. Without a GPU (SDL's
software renderer) the whole chain runs on the CPU. On the development PC every filter renders a 4K
frame within a 60 fps frame budget on either path (ctest `video-timing`).
