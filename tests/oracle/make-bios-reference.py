"""Builds a small self-authored ROM that calls the BIOS math and affine SWIs over many
inputs, runs it in mGBA (HLE BIOS) through mgba-oracle.py, and writes the inputs plus
mGBA's results to tests/runtime/data/bios/mgba-bios.dat for bios_tests.cpp.

    python make-bios-reference.py --core mgba_libretro.dll

File format (little endian): records of u32 swi, u32 in[4], u32 out[3] (r0, r1, r3 after
the call) for SWI 06h-0Ah, then u32 swi = 0x0E or 0x0F, u32 count, the raw source
structures, and mGBA's raw output structures (ObjAffineSet uses stride 8, so 32 bytes per entry).
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "runtime", "data", "bios", "mgba-bios.dat")


class Lcg:
    def __init__(self, seed):
        self.x = seed

    def next(self):
        self.x = (self.x * 1103515245 + 12345) & 0xFFFFFFFF
        return self.x


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def u32(v):
    return v & 0xFFFFFFFF


def math_cases():
    r = Lcg(2026)
    cases = []
    divs = [(-1234, 10), (1234, -10), (-0x80000000, -1), (0, 0), (1, 0), (-1, 0), (7, 7), (5, 9), (-5, 9)]
    while len(divs) < 128:
        num = s32(r.next()) >> (r.next() % 32)
        den = s32(r.next()) >> (r.next() % 32)
        if den == 0 and abs(num) > 1:
            continue
        divs.append((num, den))
    cases += [(0x06, (u32(n), u32(d), 0, 0)) for n, d in divs[:96]]
    cases += [(0x07, (u32(d), u32(n), 0, 0)) for n, d in divs[96:]]
    sq = [0, 1, 2, 3, 4, 15, 16, 17, 0xFFFFFFFF, 0x80000000, 2 << 30]
    while len(sq) < 128:
        sq.append(r.next() >> (r.next() % 32))
    cases += [(0x08, (v, 0x11111111, 0, 0x33333333)) for v in sq]
    at = [0, 0x4000, -0x4000, 0x2000, -0x2000, 1, -1, 0x7FFF, -0x8000]
    while len(at) < 128:
        at.append(s32(r.next()) >> 17)
    cases += [(0x09, (u32(v), 0x11111111, 0, 0x33333333)) for v in at]
    at2 = [(0, 0), (0x4000, 0), (-0x4000, 0), (0, 0x4000), (0, -0x4000), (0x4000, 0x4000), (-0x4000, 0x4000),
           (-0x4000, -0x4000), (0x4000, -0x4000), (0x2000, 0x1000), (-0x1000, 0x2000)]
    while len(at2) < 256:
        at2.append((s32(r.next()) >> 17, s32(r.next()) >> 17))
    cases += [(0x0A, (u32(x), u32(y), 0, 0x33333333)) for x, y in at2]
    return cases


def affine_sources():
    r = Lcg(77)
    bg, obj = b"", b""
    for i in range(32):
        angle = (i * 0x0800 + (r.next() & 0xFF)) & 0xFFFF
        sx = s32(r.next()) >> 22  # about +-2.0 in 8.8
        sy = s32(r.next()) >> 22
        ox = s32(r.next()) >> 12
        oy = s32(r.next()) >> 12
        cx = s32(r.next()) >> 25
        cy = s32(r.next()) >> 25
        bg += struct.pack("<iihhhhHH", ox, oy, cx, cy, sx, sy, angle, 0)
        obj += struct.pack("<hhHH", sx, sy, angle, 0)
    return bg, obj


class Asm:
    """Just enough ARM to drive the SWIs: literal loads, block transfers, a counted loop."""

    def __init__(self):
        self.code = []  # ints or ("lit", reg, value)

    def ldr_lit(self, reg, value):
        self.code.append(("lit", reg, value))

    def word(self, w):
        self.code.append(w)

    def assemble(self, base):
        pool_start = base + 4 * len(self.code)
        pool, out = [], []
        for i, ins in enumerate(self.code):
            if isinstance(ins, tuple):
                _, reg, value = ins
                if value not in pool:
                    pool.append(value)
                addr = pool_start + 4 * pool.index(value)
                off = addr - (base + 4 * i + 8)
                assert 0 <= off < 4096
                out.append(0xE59F0000 | (reg << 12) | off)
            else:
                out.append(ins)
        return struct.pack("<%dI" % (len(out) + len(pool)), *(out + pool))


def build_rom(cases, bg, obj):
    table = b"".join(struct.pack("<4I", *regs) for _, regs in cases)
    table_addr = 0x08002000
    bg_addr = table_addr + len(table)
    obj_addr = bg_addr + len(bg)
    a = Asm()
    out_addr = 0x02000000
    i = 0
    while i < len(cases):
        swi = cases[i][0]
        n = 1
        while i + n < len(cases) and cases[i + n][0] == swi:
            n += 1
        a.ldr_lit(8, table_addr + 16 * i)
        a.ldr_lit(9, out_addr + 12 * i)
        a.ldr_lit(10, n)
        loop = len(a.code)
        a.word(0xE8B8000F)              # ldmia r8!, {r0-r3}
        a.word(0xEF000000 | swi << 16)  # swi
        a.word(0xE8A9000B)              # stmia r9!, {r0, r1, r3}
        a.word(0xE25AA001)              # subs r10, r10, #1
        here = len(a.code)
        a.word(0x1A000000 | ((loop - here - 2) & 0xFFFFFF))  # bne loop
        i += n
    bg_out = out_addr + 12 * len(cases)
    obj_out = bg_out + 16 * 32
    a.ldr_lit(0, bg_addr)
    a.ldr_lit(1, bg_out)
    a.ldr_lit(2, 32)
    a.word(0xEF0E0000)                   # swi 0x0E
    a.ldr_lit(0, obj_addr)
    a.ldr_lit(1, obj_out)
    a.ldr_lit(2, 32)
    a.ldr_lit(3, 8)
    a.word(0xEF0F0000)                   # swi 0x0F
    a.word(0xEAFFFFFE)                   # b .
    rom = bytearray(0x2000)
    rom[0:4] = struct.pack("<I", 0xEA00002E)  # b 0x080000C0
    code = a.assemble(0x080000C0)
    assert 0xC0 + len(code) <= 0x2000
    rom[0xC0:0xC0 + len(code)] = code
    rom += table + bg + obj
    return bytes(rom), bg_out - out_addr, obj_out - out_addr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", required=True)
    args = ap.parse_args()
    cases = math_cases()
    bg, obj = affine_sources()
    rom, bg_off, obj_off = build_rom(cases, bg, obj)
    with tempfile.TemporaryDirectory() as tmp:
        rom_path = os.path.join(tmp, "bios-ref.gba")
        ew_path = os.path.join(tmp, "ewram.bin")
        with open(rom_path, "wb") as f:
            f.write(rom)
        subprocess.run([sys.executable, os.path.join(HERE, "mgba-oracle.py"), "--core", args.core, rom_path,
                        "--frames", "10", "--dump", "ewram:" + ew_path], check=True)
        with open(ew_path, "rb") as f:
            ewram = f.read()
    data = bytearray()
    for i, (swi, regs) in enumerate(cases):
        data += struct.pack("<5I", swi, *regs) + ewram[12 * i:12 * i + 12]
    data += struct.pack("<II", 0x0E, 32) + bg + ewram[bg_off:bg_off + 16 * 32]
    data += struct.pack("<II", 0x0F, 32) + obj + ewram[obj_off:obj_off + 32 * 32]
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes, %d math cases)" % (os.path.normpath(OUT), len(data), len(cases)))


if __name__ == "__main__":
    main()
