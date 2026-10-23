"""Headless mGBA reference runner (test oracle only; never linked into AnyGBA).

Loads an mGBA libretro core built from the official source release (MPL-2.0, see
tests/oracle/README.md), runs a ROM with no BIOS file so mGBA uses its HLE BIOS, and
writes screenshots, audio, and memory dumps for comparison with AnyGBA.

    python mgba_oracle.py --core mgba_libretro.dll ROM --frames N
        [--shot FRAME:out.png]... [--wav out.wav] [--dump REGION:out.bin]...
        [--keys KEY:FIRST-LAST]...

Frame k is the picture presented at the k-th VBlank (1-based), as in AnyGBA.
"""

import argparse
import ctypes as C
import os
import struct
import sys
import tempfile
import wave
import zlib

ENV_GET_SYSTEM_DIRECTORY = 9
ENV_SET_PIXEL_FORMAT = 10
ENV_GET_VARIABLE = 15
ENV_GET_SAVE_DIRECTORY = 31
ENV_SET_MEMORY_MAPS = 36 | 0x10000

KEYS = {"b": 0, "select": 2, "start": 3, "up": 4, "down": 5, "left": 6, "right": 7,
        "a": 8, "l": 10, "r": 11}
REGIONS = {"ewram": 0x02000000, "iwram": 0x03000000, "io": 0x04000000, "palette": 0x05000000,
           "vram": 0x06000000, "oam": 0x07000000}


class GameInfo(C.Structure):
    _fields_ = [("path", C.c_char_p), ("data", C.c_void_p), ("size", C.c_size_t), ("meta", C.c_char_p)]


class MemDesc(C.Structure):
    _fields_ = [("flags", C.c_uint64), ("ptr", C.c_void_p), ("offset", C.c_size_t), ("start", C.c_size_t),
                ("select", C.c_size_t), ("disconnect", C.c_size_t), ("len", C.c_size_t),
                ("addrspace", C.c_char_p)]


class MemMap(C.Structure):
    _fields_ = [("descriptors", C.POINTER(MemDesc)), ("num_descriptors", C.c_uint)]


ENV_CB = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VIDEO_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUDIO_CB = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDIO_BATCH_CB = C.CFUNCTYPE(C.c_size_t, C.POINTER(C.c_int16), C.c_size_t)
POLL_CB = C.CFUNCTYPE(None)
INPUT_CB = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)


def rgb565_to_555(p):
    return (p >> 11) | (((p >> 6) & 0x1F) << 5) | ((p & 0x1F) << 10)


def write_png(path, rgb555, width=240, height=160):
    """rgb555: list of width*height values, red in bits 0-4."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            v = rgb555[y * width + x]
            for c in (v & 0x1F, (v >> 5) & 0x1F, (v >> 10) & 0x1F):
                rows.append((c << 3) | (c >> 2))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--core", required=True)
    ap.add_argument("--frames", type=int, required=True)
    ap.add_argument("--shot", action="append", default=[], help="FRAME:out.png")
    ap.add_argument("--hash", action="append", default=[], help="FRAME: print SHA-256 of the RGB555 frame")
    ap.add_argument("--wav", help="stereo 16-bit WAV at the core's 32768 Hz")
    ap.add_argument("--dump", action="append", default=[], help="REGION:out.bin at the end")
    ap.add_argument("--keys", action="append", default=[], help="KEY:FIRST-LAST (frames, inclusive)")
    args = ap.parse_args()

    import hashlib

    shots = {}
    for s in args.shot:
        f, path = s.split(":", 1)
        shots.setdefault(int(f), []).append(path)
    hashes = {int(h) for h in args.hash}
    holds = []
    for k in args.keys:
        name, span = k.split(":", 1)
        first, last = (int(x) for x in span.split("-"))
        holds.append((KEYS[name.lower()], first, last))

    core = C.CDLL(os.path.abspath(args.core))
    sysdir = tempfile.mkdtemp(prefix="mgba-oracle-")  # empty: no BIOS file, so mGBA uses HLE
    sysdir_c = C.c_char_p(sysdir.encode())
    regions = {}
    state = {"frame": 0, "pixels": None}
    audio = bytearray()

    def env(cmd, data):
        if cmd in (ENV_GET_SYSTEM_DIRECTORY, ENV_GET_SAVE_DIRECTORY):
            C.cast(data, C.POINTER(C.c_char_p))[0] = sysdir_c.value
            return True
        if cmd == ENV_SET_PIXEL_FORMAT:
            return C.cast(data, C.POINTER(C.c_int))[0] == 2  # RGB565
        if cmd == ENV_SET_MEMORY_MAPS:
            m = C.cast(data, C.POINTER(MemMap))[0]
            for i in range(m.num_descriptors):
                d = m.descriptors[i]
                if d.ptr:
                    regions[d.start] = (d.ptr, d.len)
            return True
        return False

    def video(data, w, h, pitch):
        if not data:
            return
        assert (w, h) == (240, 160), (w, h)
        buf = (C.c_uint16 * (pitch // 2 * h)).from_address(data)
        state["pixels"] = [rgb565_to_555(buf[y * (pitch // 2) + x]) for y in range(h) for x in range(w)]

    def audio_sample(left, right):
        audio.extend(struct.pack("<hh", left, right))

    def audio_batch(data, frames):
        audio.extend(C.string_at(data, frames * 4))
        return frames

    def poll():
        pass

    def input_state(port, device, index, button):
        if port != 0 or device != 1:
            return 0
        f = state["frame"]
        return int(any(b == button and first <= f <= last for b, first, last in holds))

    cbs = [ENV_CB(env), VIDEO_CB(video), AUDIO_CB(audio_sample), AUDIO_BATCH_CB(audio_batch),
           POLL_CB(poll), INPUT_CB(input_state)]
    core.retro_set_environment(cbs[0])
    core.retro_set_video_refresh(cbs[1])
    core.retro_set_audio_sample(cbs[2])
    core.retro_set_audio_sample_batch(cbs[3])
    core.retro_set_input_poll(cbs[4])
    core.retro_set_input_state(cbs[5])
    core.retro_init()

    with open(args.rom, "rb") as f:
        rom = f.read()
    rom_buf = C.create_string_buffer(rom, len(rom))
    info = GameInfo(os.path.abspath(args.rom).encode(), C.cast(rom_buf, C.c_void_p), len(rom), None)
    if not core.retro_load_game(C.byref(info)):
        sys.exit("mGBA refused the ROM")

    for frame in range(1, args.frames + 1):
        state["frame"] = frame
        core.retro_run()
        if frame in shots or frame in hashes:
            px = state["pixels"]
            for path in shots.get(frame, []):
                write_png(path, px)
            if frame in hashes:
                raw = struct.pack("<%dH" % len(px), *px)
                print("frame %d sha256 %s" % (frame, hashlib.sha256(raw).hexdigest()))

    for d in args.dump:
        name, path = d.split(":", 1)
        ptr, length = regions[REGIONS[name]]
        with open(path, "wb") as f:
            f.write(C.string_at(ptr, length))

    if args.wav:
        with wave.open(args.wav, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(32768)
            w.writeframes(bytes(audio))

    core.retro_unload_game()
    core.retro_deinit()


if __name__ == "__main__":
    main()
