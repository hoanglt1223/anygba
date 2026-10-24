#!/usr/bin/env python3
"""Measures how much faster the music in b.wav plays than in a.wav (WAVs from a game's --wav).

Both files are reduced to log-energy envelopes (about 31 ms per step). A passage of a is
compared with b read at each candidate rate, starting anywhere within --search seconds of the
start given for b; the rate with the best normalised correlation wins. The printed ratio is
a's tempo over b's: 1.0 means b plays the music at the same tempo, 0.333 three times as fast.

Usage: music-tempo.py a.wav b.wav --start-a S --start-b S [--length S] [--search S]
                      [--expect-ratio R] [--tolerance T]
"""

import argparse
import math
import struct
import sys
import wave

HOP = 1024  # samples per envelope step (stereo frames: 1024 / 32768 Hz = 31 ms)


def envelope(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            sys.exit(f"{path}: expected 16-bit PCM")
        rate, channels = w.getframerate(), w.getnchannels()
        raw = w.readframes(w.getnframes())
    count = len(raw) // 2
    samples = struct.unpack(f"<{count}h", raw[: count * 2])
    step = HOP * channels
    log_energy = []
    for i in range(0, len(samples) - step, step):
        chunk = samples[i : i + step : 2]
        log_energy.append(math.log(sum(s * s for s in chunk) / len(chunk) + 1.0))
    return log_energy, rate / HOP


def sample(env, t):
    i = int(t)
    if i < 0 or i + 1 >= len(env):
        return None
    f = t - i
    return env[i] * (1 - f) + env[i + 1] * f


def score(a, b, start_a, start_b, length, ratio):
    xs, ys = [], []
    for k in range(length):
        y = sample(b, start_b + k * ratio)
        if y is None:
            return -1.0
        xs.append(a[start_a + k])
        ys.append(y)
    mx, my = sum(xs) / length, sum(ys) / length
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    return sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else -1.0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--start-a", type=float, required=True)
    p.add_argument("--start-b", type=float, required=True)
    p.add_argument("--length", type=float, default=5.0, help="seconds of a to compare")
    p.add_argument("--search", type=float, default=3.5, help="seconds around --start-b to try")
    p.add_argument("--min-ratio", type=float, default=0.25)
    p.add_argument("--max-ratio", type=float, default=1.5)
    p.add_argument("--expect-ratio", type=float)
    p.add_argument("--tolerance", type=float, default=0.03)
    args = p.parse_args()
    a, fps = envelope(args.a)
    b, fps_b = envelope(args.b)
    if fps != fps_b:
        sys.exit("the files have different sample rates")
    start_a = int(args.start_a * fps)
    length = int(args.length * fps)
    if start_a + length > len(a):
        sys.exit(f"{args.a} is shorter than --start-a + --length")

    def best_over(ratios, offsets):
        best = (-2.0, 0.0, 0)
        for r in ratios:
            for o in offsets:
                s = score(a, b, start_a, args.start_b * fps + o, length, r)
                if s > best[0]:
                    best = (s, r, o)
        return best

    # Coarse: ratios on a log grid, starts every other step; then refine around the winner.
    steps = int(math.log(args.max_ratio / args.min_ratio) / math.log(1.02)) + 1
    ratios = [args.min_ratio * 1.02**i for i in range(steps)]
    offsets = range(-int(args.search * fps), int(args.search * fps) + 1, 2)
    corr, ratio, offset = best_over(ratios, offsets)
    corr, ratio, offset = best_over([ratio * (1 + d / 1000) for d in range(-20, 21)], range(offset - 2, offset + 3))
    print(f"tempo ratio {ratio:.3f} (correlation {corr:.2f}, b starts at {(args.start_b * fps + offset) / fps:.2f} s)")
    if args.expect_ratio is not None and abs(ratio - args.expect_ratio) > args.tolerance * args.expect_ratio:
        sys.exit(f"tempo ratio {ratio:.3f}, expected {args.expect_ratio}")


if __name__ == "__main__":
    main()
