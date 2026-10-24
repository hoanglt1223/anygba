#!/usr/bin/env sh
# Builds the tonc demos from source with devkitARM (Docker) so discovery can be measured
# against their ELF symbol tables. tonc-bin ships ROMs only.
# usage: tests/build-tonc-elves.sh DIR
# then:  ANYGBA_ELF_DIR=DIR/code anygba-tests -tc="*ANYGBA_ELF_DIR*"
set -eu

[ $# -eq 1 ] || { sed -n '4,5p' "$0" >&2; exit 2; }
dir=$1
url=https://www.coranac.com/files/tonc-code.zip
sha=7073fe253882773d84a5928d1c0f9dc0cf98fce7fc6478770c45a59992771b86
image=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5

mkdir -p "$dir"
cd "$dir"
if [ ! -f tonc-code.zip ]; then
    curl -fsSL -o tonc-code.zip.part "$url"
    mv tonc-code.zip.part tonc-code.zip
fi
echo "$sha  tonc-code.zip" | sha256sum -c -
rm -rf code
unzip -q tonc-code.zip

# The 2013 sources predate C23 (where bool/false/true are keywords) and the current newlib
# devoptab signatures.
types=code/tonclib/include/tonc_types.h
sed -i 's/^typedef enum { false, true } bool;$/#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L\ntypedef enum { false, true } bool;\n#endif/' "$types"
iohook=$(find code/tonclib -name tte_iohook.c)
sed -i '1i #pragma GCC diagnostic warning "-Wincompatible-pointer-types"' "$iohook"
rm -f code/tonclib/lib/libtonc.a

host_dir=$(pwd)
if command -v cygpath >/dev/null 2>&1; then
    host_dir=$(cygpath -w "$host_dir")
fi
MSYS_NO_PATHCONV=1 docker run --rm -v "$host_dir:/src" -w /src/code "$image" \
    sh -c "make -C tonclib && make -k build_all"
echo "built $(find code -name '*.elf' | wc -l) ELFs in $dir/code"
