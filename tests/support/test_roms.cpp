#include "support/test_roms.h"

#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace anygba::test {

namespace {

constexpr std::uint32_t kTableAddress = 0x08000400;

// ARM program at 0x080000C0 (each word with its assembly):
constexpr std::uint32_t kProgram[] = {
    0xE59F404C,  // C0  ldr   r4, =table
    0xE8B40007,  // C4  loop: ldmia r4!, {r0-r2}      address, value, size | frames << 8
    0xE3500000,  // C8  cmp   r0, #0
    0x0A00000F,  // CC  beq   done
    0xE20230FF,  // D0  and   r3, r2, #0xFF
    0xE3530004,  // D4  cmp   r3, #4
    0x05801000,  // D8  streq r1, [r0]
    0x11C010B0,  // DC  strneh r1, [r0]
    0xE1B02422,  // E0  movs  r2, r2, lsr #8
    0x0AFFFFF6,  // E4  beq   loop
    0xE59F5028,  // E8  ldr   r5, =0x04000006
    0xE1D560B0,  // EC  w1: ldrh r6, [r5]            wait for VCOUNT == 160
    0xE35600A0,  // F0  cmp   r6, #160
    0x1AFFFFFC,  // F4  bne   w1
    0xE1D560B0,  // F8  w2: ldrh r6, [r5]            ... and for it to move on
    0xE35600A0,  // FC  cmp   r6, #160
    0x0AFFFFFC,  // 100 beq   w2
    0xE2522001,  // 104 subs  r2, r2, #1
    0x1AFFFFF7,  // 108 bne   w1
    0xEAFFFFEC,  // 10C b     loop
    0xEAFFFFFE,  // 110 done: b done
    kTableAddress,
    0x04000006,
};

void put32(std::vector<std::uint8_t>& rom, std::size_t off, std::uint32_t v) { std::memcpy(rom.data() + off, &v, 4); }

// Registers used by the sound scripts.
constexpr std::uint32_t kSound1L = 0x04000060, kSound1H = 0x04000062, kSound1X = 0x04000064;
constexpr std::uint32_t kSound2L = 0x04000068, kSound2H = 0x0400006C;
constexpr std::uint32_t kSound3L = 0x04000070, kSound3H = 0x04000072, kSound3X = 0x04000074;
constexpr std::uint32_t kSound4L = 0x04000078, kSound4H = 0x0400007C;
constexpr std::uint32_t kSoundCntL = 0x04000080, kSoundCntH = 0x04000082, kSoundCntX = 0x04000084;
constexpr std::uint32_t kWaveRam = 0x04000090;
constexpr std::uint32_t kPlayFrames = 60;

std::vector<RegWrite> sound_on(std::uint16_t cnt_l, std::uint16_t cnt_h) {
    return {{kSoundCntX, 0x0080}, {kSoundCntL, cnt_l}, {kSoundCntH, cnt_h}};
}

std::vector<RegWrite> triangle_wave_ram() {
    // Select bank 1 for playback so the CPU writes bank 0, then play bank 0.
    std::vector<RegWrite> s = {{kSound3L, 0x0040}};
    const std::uint8_t digits[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                     15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    for (std::uint32_t i = 0; i < 16; i += 2) {
        const auto byte = [&digits](std::uint32_t b) { return static_cast<std::uint32_t>((digits[2 * b] << 4) | digits[2 * b + 1]); };
        s.push_back({kWaveRam + i, byte(i) | (byte(i + 1) << 8)});
    }
    s.push_back({kSound3L, 0x0080});
    return s;
}

std::vector<std::uint8_t> sine_blob(std::size_t bytes) {
    std::vector<std::uint8_t> blob(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        const double v = 100.0 * std::sin(2.0 * std::numbers::pi * static_cast<double>(i % 32) / 32.0);
        blob[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(v)));
    }
    return blob;
}

std::vector<RegWrite> concat(std::vector<std::vector<RegWrite>> parts) {
    std::vector<RegWrite> out;
    for (auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

}  // namespace

std::vector<std::uint8_t> make_script_rom(const std::vector<RegWrite>& script, const std::vector<std::uint8_t>& blob) {
    constexpr std::size_t kTable = kTableAddress - 0x08000000;
    constexpr std::size_t kBlob = kBlobAddress - 0x08000000;
    if ((script.size() + 1) * 12 > kBlob - kTable) {
        throw std::length_error("script ROM: too many register writes");
    }
    std::vector<std::uint8_t> rom(kBlob + blob.size(), 0);
    put32(rom, 0, 0xEA00002E);  // b 0x080000C0
    for (std::size_t i = 0; i < std::size(kProgram); ++i) {
        put32(rom, 0xC0 + 4 * i, kProgram[i]);
    }
    std::size_t at = kTable;
    for (const RegWrite& w : script) {
        put32(rom, at, w.addr);
        put32(rom, at + 4, w.value);
        put32(rom, at + 8, static_cast<std::uint32_t>(w.size) | (w.frames << 8));
        at += 12;
    }
    std::memcpy(rom.data() + kBlob, blob.data(), blob.size());
    return rom;
}

std::vector<TestRom> apu_test_roms() {
    std::vector<TestRom> roms;
    const auto add = [&roms](std::string name, std::vector<RegWrite> script, std::vector<std::uint8_t> blob = {}) {
        script.back().frames = kPlayFrames;
        roms.push_back({std::move(name), make_script_rom(script, blob)});
    };
    // Square 1: 440 Hz, 50% duty, constant volume 15.
    add("square1", concat({sound_on(0x1177, 0x0002), {{kSound1L, 0x0000}, {kSound1H, 0xF080}, {kSound1X, 0x8000 | 1750}}}));
    // Square 1 sweeping up from 128 Hz with a decaying envelope, 12.5% duty.
    add("square1-sweep",
        concat({sound_on(0x1177, 0x0002), {{kSound1L, 0x0072}, {kSound1H, 0xF700}, {kSound1X, 0x8000 | 1024}}}));
    // Square 2: 880 Hz, 25% duty, volume 10 decaying.
    add("square2", concat({sound_on(0x2277, 0x0002), {{kSound2L, 0xA340}, {kSound2H, 0x8000 | 1899}}}));
    // Wave: a triangle at 512 Hz, 100% volume.
    add("wave", concat({sound_on(0x4477, 0x0002), triangle_wave_ram(), {{kSound3H, 0x2000}, {kSound3X, 0x8000 | 1920}}}));
    // Noise: 15-bit LFSR, r=1 s=4 (a step every 1024 cycles), constant volume.
    add("noise", concat({sound_on(0x8877, 0x0002), {{kSound4L, 0xF000}, {kSound4H, 0x8000 | 0x41}}}));
    // Noise: 7-bit LFSR (tonal), r=4 s=5 (a step every 8192 cycles), rising envelope from 4.
    add("noise7", concat({sound_on(0x8877, 0x0002), {{kSound4L, 0x4E00}, {kSound4H, 0x8000 | 0x5C}}}));
    // FIFO A: a 512 Hz sine at 16384 samples/s (timer 0), DMA 1, 100%, both sides.
    add("fifo-a",
        concat({sound_on(0x0077, 0x0B06),
                {{0x040000BC, kBlobAddress, 4},
                 {0x040000C0, 0x040000A0, 4},
                 {0x040000C6, 0xB600},
                 {0x04000100, 0xFC00},
                 {0x04000102, 0x0080}}}),
        sine_blob(48 * 1024));
    // Mix: square 1 left only, wave right only, FIFO B (timer 1, 8192 samples/s, 50%) on both.
    add("mix",
        concat({sound_on(0x1477, 0xF002), triangle_wave_ram(),
                {{kSound1L, 0x0000},
                 {kSound1H, 0xC080},
                 {kSound1X, 0x8000 | 1750},
                 {kSound3H, 0x4000},
                 {kSound3X, 0x8000 | 1920},
                 {0x040000C8, kBlobAddress, 4},
                 {0x040000CC, 0x040000A4, 4},
                 {0x040000D2, 0xB600},
                 {0x04000104, 0xF800},
                 {0x04000106, 0x0080}}}),
        sine_blob(48 * 1024));
    return roms;
}

}  // namespace anygba::test
