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

## Test pictures

The golden images in `tests/golden/video/` come from a test frame drawn by code in
`tests/runtime/video_tests.cpp`; no game graphics are involved.
