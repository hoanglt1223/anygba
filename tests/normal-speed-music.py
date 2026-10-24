#!/usr/bin/env python3
"""Checks NormalSpeedMusic on a generated MP2K game (e.g. one built from your own Pokemon
Emerald dump): with it on, music recorded at a higher speed has the 100% tempo; with it off,
the tempo rises with the speed. Needs the game only, no ROM in the repository.

  python tests/normal-speed-music.py --game em.exe [--speed 300] [--music-start 180] [--at 480]

--music-start is the frame (at 100%) where the passage's song starts, --at the frame where
the compared passage begins; the defaults fit Pokemon Emerald's intro. Files go to --work.
"""

import argparse
import contextlib
import importlib
import io
import math
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
tempo = importlib.import_module("music-tempo")


def run(game, work, name, frames, extra):
    wav = work / f"{name}.wav"
    env = dict(os.environ, ANYGBA_DATA_DIR=str(work / "data"))
    cmd = [str(game), "--headless", "--no-save", "--no-miss-log", "--frames", str(frames), "--wav", str(wav), *extra]
    result = subprocess.run(cmd, cwd=work, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"{' '.join(cmd)} failed:\n{result.stdout}{result.stderr}")
    return wav


def measure(a, b, start_a, start_b, length):
    sys.argv = ["music-tempo.py", str(a), str(b), "--start-a", f"{start_a}", "--start-b", f"{start_b}",
                "--length", f"{length}"]
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        tempo.main()
    text = out.getvalue().strip()
    print(f"  {b.name}: {text}")
    return float(text.split()[2])


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--game", required=True, type=Path)
    p.add_argument("--work", type=Path, default=Path("out/normal-speed-music"))
    p.add_argument("--speed", type=int, default=300, choices=[200, 300, 400])
    p.add_argument("--music-start", type=int, default=180)
    p.add_argument("--at", type=int, default=480)
    p.add_argument("--length", type=float, default=5.0)
    p.add_argument("--tolerance", type=float, default=0.03)
    args = p.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    game = args.game.resolve()
    (work / "on.ini").write_bytes(b"[player]\nNormalSpeedMusic = on\n")
    s = args.speed / 100
    frames = args.at + int(args.length * 60) + 120
    ref = run(game, work, "speed100", frames, [])
    on = run(game, work, f"speed{args.speed}-on", math.ceil(frames * s), ["--speed", str(args.speed), "--input", str(work / "on.ini")])
    off = run(game, work, f"speed{args.speed}-off", math.ceil(frames * s), ["--speed", str(args.speed)])
    start_a = args.at / 60
    # Where the passage plays in each recording: the song starts at music_start / speed, then
    # runs at 1x (on) or at the speed (off).
    start_on = args.music_start / 60 / s + (args.at - args.music_start) / 60
    start_off = args.at / 60 / s
    ratio_on = measure(ref, on, start_a, start_on, args.length)
    ratio_off = measure(ref, off, start_a, start_off, args.length)
    failures = []
    if abs(ratio_on - 1.0) > args.tolerance:
        failures.append(f"NormalSpeedMusic on: tempo ratio {ratio_on:.3f}, expected 1.0")
    if abs(ratio_off - 1 / s) > args.tolerance:
        failures.append(f"NormalSpeedMusic off: tempo ratio {ratio_off:.3f}, expected {1 / s:.3f}")
    if failures:
        sys.exit("\n".join(failures))
    print(f"ok: music at {args.speed}% keeps the 100% tempo with NormalSpeedMusic on")


if __name__ == "__main__":
    main()
