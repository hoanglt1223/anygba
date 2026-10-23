# BIOS reference data

Used by `tests/runtime/bios_tests.cpp`. Nothing here comes from a BIOS image or a ROM.

## Decompression vectors (`*.cmp`)

The uncompressed input is self-authored: `make_input(n)` in `bios_tests.cpp` (text, a ramp, LCG bytes,
and runs in 64-byte blocks). It was written to a file and compressed with the GBA compressors of
[grit](https://github.com/devkitPro/grit) v0.10.0 (`libgrit/cprs_lz.cpp`, `cprs_rle.cpp`, `cprs_huff.cpp`),
built locally with a small driver that calls `lz77gba_compress`, `rle8gba_compress`, and `huffEncode`
(4-bit and 8-bit). Only the compressed output is committed.

| File | Format | Decompressed size |
|---|---|---|
| `lz77-1024.cmp`, `lz77-1021.cmp` | LZ77 (type 1) | 1024, 1021 |
| `rle-1024.cmp`, `rle-1021.cmp` | RLE (type 3) | 1024, 1021 |
| `huff8-1024.cmp`, `huff4-1024.cmp` | Huffman (type 2), 8-bit and 4-bit units | 1024 |

## mGBA results (`mgba-bios.dat`)

`tests/oracle/make-bios-reference.py` builds a self-authored ROM that calls Div, DivArm, Sqrt, ArcTan,
ArcTan2, BgAffineSet and ObjAffineSet over 640 register inputs and 32 affine structures, runs it in mGBA
0.10.5 (libretro core built from the official source tag, no BIOS file, so mGBA's HLE BIOS answers), and
stores inputs and results. The file format is described at the top of that script; see
`tests/oracle/README.md` for building the core.
