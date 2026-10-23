"""Regenerates tests/golden/ppu/<name>.png from tests/golden/ppu/cases.txt with mGBA.

    python make-ppu-goldens.py --core mgba_libretro.dll [name ...]

Each PNG is mGBA's frame at the case's frame number (see cases.txt for how ppu_tests.cpp
matches it). Needs the test corpus (tests/fetch-corpus.*).
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
GOLDEN = os.path.join(ROOT, "golden", "ppu")


def cases():
    with open(os.path.join(GOLDEN, "cases.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if not line:
                continue
            name, rom, frame = line[0], line[1], int(line[2])
            keys = [t for t in line[3:] if ":" in t]
            yield name, rom, frame, keys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", required=True)
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()
    for name, rom, frame, keys in cases():
        if args.names and name not in args.names:
            continue
        cmd = [sys.executable, os.path.join(HERE, "mgba-oracle.py"), "--core", args.core,
               os.path.join(ROOT, "corpus", rom), "--frames", str(frame + 1),
               "--shot", "%d:%s" % (frame, os.path.join(GOLDEN, name + ".png"))]
        for f in (frame - 1, frame, frame + 1):
            cmd += ["--hash", str(f)]
        for k in keys:
            cmd += ["--keys", k]
        out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.split()
        hashes = out[3::4]
        static = "static" if len(set(hashes)) == 1 else "animated"
        print("%-18s frame %-4d %s" % (name, frame, static))


if __name__ == "__main__":
    main()
