# Third-party code and algorithm provenance

AnyGBA is MIT-licensed. Libraries it builds against are listed with their licences in
[docs/dev/ARCHITECTURE.md](docs/dev/ARCHITECTURE.md#dependencies); none of their code is copied into this
repository.

## Video filters (`runtime/src/video/`)

Every filter is written for AnyGBA from published descriptions of the algorithm. No code was copied
or translated from the implementations named here, several of which are under copyleft licences.

| Filter | Algorithm by | Source of the description | Notes |
|---|---|---|---|
| Scale2x, Scale3x, Scale4x | Andrea Mazzoleni (AdvanceMAME), after Eric Johnston's EPX | The Scale2x project's algorithm page (the reference code is GPL-2) | Exact rules as described; Scale4x is Scale2x applied twice, as specified. |
| Eagle | Dirk Stevens | Public descriptions of the Eagle corner rule | Super Eagle here adds a half-blend where only the two edge neighbours agree. |
| 2xSaI, Super 2xSaI, Super Eagle | Derek Liauw Kie Fa (Kreed) | Public descriptions of 2xSaI's block rules and neighbour voting | Not bit-identical to Kreed's code; Super 2xSaI here weights interpolation toward the centre pixel. |
| hq2x, hq3x, hq4x | Maxim Stepin | The hqx description: YUV comparison with thresholds 48/7/6 and corner interpolation | Independent of the LGPL reference and its pattern tables; output differs in detail. |
| xBR | Hyllian | Hyllian's xBR tutorial: weighted edge distances in a 5x5 window, level-2 shallow/steep edges | Corner cuts blended by covered area. |
| xBRZ | Zenju | The xBRZ write-up: dominant-direction (3.6) and steep/shallow (2.2) thresholds | Independent of the GPL-3 reference; output differs in detail. |
| GBA LCD colour | Talarubi and Near (byuu), colour emulation for higan | The published formula: LCD gamma 4.0, a channel-mixing matrix, output gamma 2.2 at 255/280 | Formula reimplemented; the higan code (ISC) is not used. |
| Sharp bilinear, LCD grid, scanlines, CRT, ghosting, debanding | Common techniques | — | Written for AnyGBA. |

## HD rendering and AI upscaling (`runtime/src/video/hd/`, `tools/hdpack/`)

- The HD renderer, pack format, dumper, widescreen rules and the `pokeemerald-field` provider are
  written for AnyGBA. The provider follows the map, tileset and metatile layout documented by the
  [pret/pokeemerald](https://github.com/pret/pokeemerald) decompilation; no decompilation code is
  copied, and the addresses in `widescreen/BPEE.toml` were checked by reading a running game's memory.
- [ONNX Runtime](https://github.com/microsoft/onnxruntime) (MIT): the C API headers
  `onnxruntime_c_api.h` and `onnxruntime_ep_c_api.h` of v1.24.4 are downloaded at configure time
  (SHA-256 pinned in `cmake/dependencies.cmake`); the library is loaded at run time and, on Windows,
  downloaded on first use from nuget.org (Microsoft.ML.OnnxRuntime.DirectML 1.24.4, pinned), never
  committed. `anygba hdpack fetch-runtime --directml` can also fetch Microsoft.AI.DirectML 1.15.4
  (Microsoft's DirectML licence).
- Models: Real-ESRGAN weights (BSD-3-Clause, Xintao Wang et al.), as ONNX exports by Qualcomm AI Hub;
  downloaded on first use, SHA-256 pinned, never committed ([HD-PACKS.md](docs/user/HD-PACKS.md#ai-upscaling)).
- PNG files are read and written with SDL 3.4 (zlib licence), already a dependency.

## In-game menu (`runtime/src/ui/`)

- Font: Noto Sans Regular and SemiBold 2.015 (the `NotoSans-v2.015` release of
  [notofonts/latin-greek-cyrillic](https://github.com/notofonts/latin-greek-cyrillic), unhinted TTF),
  SIL Open Font License 1.1, copyright 2022 The Noto Project Authors; the licence is in
  `runtime/src/ui/fonts/OFL.txt`. The files are subsets made with fontTools `pyftsubset` (Basic Latin,
  Latin-1, Latin Extended-A, U+01A0-01B0, combining marks U+0300-0323, Latin Extended Additional
  U+1EA0-1EF9 and general punctuation; no layout tables, no hinting), enough for English and Vietnamese.
  They are compiled into the runtime.
- `runtime/src/ui/anyui/` (rasteriser, TrueType reader, icons, menu layout) is written for AnyGBA and
  shared, MIT-licensed, with AnyPSP. The area-coverage line rasteriser follows the published description
  of the signed-area accumulation technique (as in font-rs); no code is copied.

## Test pictures

The golden images in `tests/golden/video/` come from a test frame drawn by code in
`tests/runtime/video_tests.cpp`; no game graphics are involved. Those in `tests/golden/menu/` are the
menu drawn over a scene drawn by `tests/runtime/menu_view_tests.cpp` (the 2x ones kept at half size).
