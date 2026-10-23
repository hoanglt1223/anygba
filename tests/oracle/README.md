# mGBA reference oracle

Reference data for the BIOS, PPU and APU tests comes from [mGBA](https://mgba.io) run headless. mGBA is
MPL-2.0; it is used only as an external program that produces expected outputs. No mGBA code is copied
into AnyGBA, and nothing here is built by AnyGBA's CMake.

## Building the core

The oracle is mGBA's libretro core built from the official 0.10.5 source tag, with every optional
dependency off (no BIOS file is ever given to it, so it uses its own HLE BIOS):

```sh
git clone --depth 1 --branch 0.10.5 https://github.com/mgba-emu/mgba.git
cmake -S mgba -B mgba-out -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSKIP_LIBRARY=ON -DBUILD_LIBRETRO=ON \
  -DBUILD_QT=OFF -DBUILD_SDL=OFF -DUSE_FFMPEG=OFF -DUSE_ZLIB=OFF -DUSE_MINIZIP=OFF -DUSE_PNG=OFF \
  -DUSE_LIBZIP=OFF -DUSE_SQLITE3=OFF -DUSE_ELF=OFF -DUSE_LUA=OFF -DUSE_LZMA=OFF -DUSE_DISCORD_RPC=OFF \
  -DUSE_EDITLINE=OFF -DUSE_GDB_STUB=OFF -DUSE_DEBUGGERS=OFF -DBUILD_GL=OFF -DBUILD_GLES2=OFF \
  -DBUILD_GLES3=OFF -DUSE_EPOXY=OFF
cmake --build mgba-out --config Release --target mgba_libretro
```

On Windows with Visual Studio 2022 this produces `mgba-out/Release/mgba_libretro.dll`.

## Scripts

All scripts use only the Python standard library.

| Script | Produces |
|---|---|
| `mgba-oracle.py` | Runs a ROM for N frames; writes RGB555 frame hashes, PNG screenshots, a WAV of the audio stream (32768 Hz stereo), and memory dumps. Frame k is the picture at the k-th VBlank, as in AnyGBA. |
| `make-bios-reference.py` | `tests/runtime/data/bios/mgba-bios.dat` (see that directory's README) |

Example:

```sh
python tests/oracle/mgba-oracle.py --core mgba_libretro.dll tests/corpus/tonc/bin/sbb_reg.gba \
  --frames 60 --shot 60:sbb_reg.png --hash 60
```

## Known mGBA HLE differences

- jsmolka's `bios.gba` fails test 1 in mGBA's HLE mode (BIOS reads after boot); AnyGBA passes all four.
- BgAffineSet/ObjAffineSet: mGBA computes in float, the BIOS in 1.14 fixed point. AnyGBA follows the
  fixed-point form, so matrix entries may differ from mGBA by one 8.8 step.
