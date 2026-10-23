"""Regenerates tests/runtime/data/apu/<name>.pcmz from tests/runtime/data/apu/cases.txt with mGBA.

    python make-apu-references.py --core mgba_libretro.dll --test-roms <out>/bin/Release/anygba-test-roms.exe

anygba-test-roms writes the self-authored sound ROMs; each is played for 62 frames in mGBA and the
part of its output that cases.txt names is stored (see that file for the format).
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import wave
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.normpath(os.path.join(HERE, "..", "runtime", "data", "apu"))
WINDOW = 1024
BLOCK = 512
FRAMES = 62


def cases():
    with open(os.path.join(DATA, "cases.txt"), encoding="utf-8") as f:
        for line in f:
            t = line.split("#", 1)[0].split()
            if t:
                extra = dict(kv.split("=", 1) for kv in t[5:])
                yield t[0], int(t[1]), int(t[2]), t[3], extra


def onset(samples):
    """First stereo frame where either channel moves more than 300 from the first frame."""
    l0, r0 = samples[0], samples[1]
    for i in range(0, len(samples), 2):
        if abs(samples[i] - l0) > 300 or abs(samples[i + 1] - r0) > 300:
            return i // 2
    raise SystemExit("no sound found")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", required=True)
    ap.add_argument("--test-roms", required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([os.path.abspath(args.test_roms), tmp], check=True, capture_output=True)
        for name, start, count, check, extra in cases():
            wav = os.path.join(tmp, name + ".wav")
            rom = (os.path.join(HERE, "..", "corpus", extra["rom"]) if "rom" in extra
                   else os.path.join(tmp, "apu-%s.gba" % name))
            cmd = [sys.executable, os.path.join(HERE, "mgba-oracle.py"), "--core", args.core, rom,
                   "--frames", extra.get("frames", str(FRAMES)), "--wav", wav]
            for k in filter(None, extra.get("keys", "").split(",")):
                cmd += ["--keys", k]
            subprocess.run(cmd, check=True)
            with wave.open(wav) as w:
                n = w.getnframes()
                samples = struct.unpack("<%dh" % (2 * n), w.readframes(n))
            first = onset(samples) + start
            length = count * (WINDOW if check == "corr" else BLOCK)
            if first + length > n:
                raise SystemExit("%s: capture too short" % name)
            cut = samples[2 * first:2 * (first + length)]
            with open(os.path.join(DATA, name + ".pcmz"), "wb") as f:
                f.write(zlib.compress(struct.pack("<%dh" % len(cut), *cut), 9))
            print("%-14s onset %d, %d frames stored" % (name, first - start, length))


if __name__ == "__main__":
    main()
