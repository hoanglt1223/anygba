#include "anygba/system.hpp"
#include "bios/bios.h"
#include "support/corpus.h"

#include <doctest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using anygba::System;

namespace {

const std::filesystem::path kData = std::filesystem::path(ANYGBA_TEST_DATA_DIR) / "bios";

std::vector<std::uint8_t> rom_with(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> words) {
    std::vector<std::uint8_t> rom(0x400, 0);
    for (const auto& [off, w] : words) {
        std::memcpy(rom.data() + off, &w, 4);
    }
    return rom;
}

anygba::Options quiet() {
    anygba::Options o;
    o.log_misses = false;
    return o;
}

struct Bios {
    System sys{rom_with({{0x00, 0xEAFFFFFE}}), nullptr, quiet()};  // b .
    anygba_cpu& c = sys.cpu();

    void call(std::uint32_t number, std::uint32_t r0 = 0, std::uint32_t r1 = 0, std::uint32_t r2 = 0,
              std::uint32_t r3 = 0) {
        c.r[0] = r0;
        c.r[1] = r1;
        c.r[2] = r2;
        c.r[3] = r3;
        sys.swi(number);
    }
    void poke(std::uint32_t addr, const std::vector<std::uint8_t>& bytes) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            sys.write8(addr + static_cast<std::uint32_t>(i), bytes[i]);
        }
    }
    std::vector<std::uint8_t> peek(std::uint32_t addr, std::size_t n) {
        std::vector<std::uint8_t> out(n);
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = static_cast<std::uint8_t>(sys.read8(addr + static_cast<std::uint32_t>(i)));
        }
        return out;
    }
};

// Same generator as the one that produced the committed *.cmp vectors (see data/bios/README.md).
std::vector<std::uint8_t> make_input(std::size_t n) {
    static constexpr char kText[] = "AnyGBA HLE BIOS test vector. ";
    std::uint32_t x = 0x1234567;
    std::vector<std::uint8_t> data;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        switch ((i / 64) % 4) {
        case 0: data.push_back(static_cast<std::uint8_t>(kText[i % (sizeof kText - 1)])); break;
        case 1: data.push_back(static_cast<std::uint8_t>(i)); break;
        case 2: data.push_back(static_cast<std::uint8_t>(x >> 24)); break;
        default: data.push_back(static_cast<std::uint8_t>(i / 64)); break;
        }
    }
    return data;
}

std::uint32_t u32_at(const std::vector<std::uint8_t>& d, std::size_t off) {
    std::uint32_t v;
    std::memcpy(&v, d.data() + off, 4);
    return v;
}

std::int32_t s16_at(const std::vector<std::uint8_t>& d, std::size_t off) {
    std::int16_t v;
    std::memcpy(&v, d.data() + off, 2);
    return v;
}

}  // namespace

TEST_CASE("BIOS Div, Sqrt and ArcTan match mGBA's HLE BIOS bit for bit") {
    const auto data = anygba::test::read_file(kData / "mgba-bios.dat");
    Bios b;
    std::size_t off = 0;
    int checked = 0;
    while (off + 32 <= data.size() && u32_at(data, off) >= 0x06 && u32_at(data, off) <= 0x0A) {
        const std::uint32_t swi = u32_at(data, off);
        b.call(swi, u32_at(data, off + 4), u32_at(data, off + 8), u32_at(data, off + 12), u32_at(data, off + 16));
        INFO("SWI 0x" << std::hex << swi << " r0=0x" << u32_at(data, off + 4) << " r1=0x" << u32_at(data, off + 8));
        CHECK(b.c.r[0] == u32_at(data, off + 20));
        CHECK(b.c.r[1] == u32_at(data, off + 24));
        CHECK(b.c.r[3] == u32_at(data, off + 28));
        off += 32;
        ++checked;
    }
    CHECK(checked == 640);
}

TEST_CASE("BIOS BgAffineSet and ObjAffineSet agree with mGBA within its float rounding") {
    // mGBA computes these in float; the BIOS (and AnyGBA) use a 1.14 sine table. Matrix
    // entries may differ by one 8.8 step; the start point by one step per unit of the
    // display centre offset that multiplies it.
    const auto data = anygba::test::read_file(kData / "mgba-bios.dat");
    std::size_t off = 640 * 32;
    REQUIRE(u32_at(data, off) == 0x0E);
    REQUIRE(u32_at(data, off + 4) == 32);
    off += 8;
    Bios b;
    const std::vector<std::uint8_t> bg_src(data.begin() + static_cast<std::ptrdiff_t>(off),
                                           data.begin() + static_cast<std::ptrdiff_t>(off + 32 * 20));
    const std::vector<std::uint8_t> bg_ref(data.begin() + static_cast<std::ptrdiff_t>(off + 32 * 20),
                                           data.begin() + static_cast<std::ptrdiff_t>(off + 32 * 36));
    b.poke(0x02000000, bg_src);
    b.call(0x0E, 0x02000000, 0x02010000, 32);
    const auto bg_out = b.peek(0x02010000, 32 * 16);
    int exact = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        for (std::size_t k = 0; k < 4; ++k) {
            const std::int32_t got = s16_at(bg_out, i * 16 + k * 2);
            const std::int32_t want = s16_at(bg_ref, i * 16 + k * 2);
            CHECK(std::abs(got - want) <= 1);
            exact += got == want;
        }
        const std::int32_t cx = s16_at(bg_src, i * 20 + 8);
        const std::int32_t cy = s16_at(bg_src, i * 20 + 10);
        for (std::size_t k = 0; k < 2; ++k) {
            const auto got = static_cast<std::int32_t>(u32_at(bg_out, i * 16 + 8 + k * 4));
            const auto want = static_cast<std::int32_t>(u32_at(bg_ref, i * 16 + 8 + k * 4));
            CHECK(std::abs(got - want) <= std::abs(cx) + std::abs(cy) + 1);
        }
    }
    MESSAGE("BgAffineSet matrix entries identical to mGBA: " << exact << " of 128");
    CHECK(exact >= 64);

    off += 32 * 36;
    REQUIRE(u32_at(data, off) == 0x0F);
    off += 8;
    const std::vector<std::uint8_t> obj_src(data.begin() + static_cast<std::ptrdiff_t>(off),
                                            data.begin() + static_cast<std::ptrdiff_t>(off + 32 * 8));
    const std::vector<std::uint8_t> obj_ref(data.begin() + static_cast<std::ptrdiff_t>(off + 32 * 8),
                                            data.begin() + static_cast<std::ptrdiff_t>(off + 32 * 8 + 32 * 32));
    b.poke(0x02000000, obj_src);
    b.call(0x0F, 0x02000000, 0x02020000, 32, 8);
    const auto obj_out = b.peek(0x02020000, 32 * 32);
    for (std::size_t i = 0; i < 32 * 32; i += 8) {
        CHECK(std::abs(s16_at(obj_out, i) - s16_at(obj_ref, i)) <= 1);
        CHECK(s16_at(obj_out, i + 2) == 0);  // stride 8 leaves the gaps alone
    }
}

TEST_CASE("BIOS Div follows GBATEK, including division by zero") {
    Bios b;
    b.call(0x06, static_cast<std::uint32_t>(-1234), 10);
    CHECK(static_cast<std::int32_t>(b.c.r[0]) == -123);
    CHECK(static_cast<std::int32_t>(b.c.r[1]) == -4);
    CHECK(b.c.r[3] == 123);
    b.call(0x07, 10, static_cast<std::uint32_t>(-1234));  // DivArm swaps the operands
    CHECK(static_cast<std::int32_t>(b.c.r[0]) == -123);
    CHECK_THROWS_WITH_AS(b.call(0x06, 5, 0), doctest::Contains("hangs the real BIOS"), anygba::Error);
}

TEST_CASE("BIOS decompressors reproduce data compressed by grit") {
    struct Vector {
        const char* file;
        std::uint32_t swi_wram;
        std::uint32_t swi_vram;  // 0: no VRAM variant
        std::size_t size;
    };
    const Vector vectors[] = {
        {"lz77-1024.cmp", 0x11, 0x12, 1024}, {"lz77-1021.cmp", 0x11, 0x12, 1021},
        {"rle-1024.cmp", 0x14, 0x15, 1024},  {"rle-1021.cmp", 0x14, 0x15, 1021},
        {"huff8-1024.cmp", 0x13, 0, 1024},   {"huff4-1024.cmp", 0x13, 0, 1024},
    };
    for (const Vector& v : vectors) {
        CAPTURE(v.file);
        const auto packed = anygba::test::read_file(kData / v.file);
        const auto expected = make_input(v.size);
        Bios b;
        b.poke(0x02000000, packed);
        b.call(v.swi_wram, 0x02000000, 0x03000000);
        CHECK(b.peek(0x03000000, v.size) == expected);
        if (v.swi_vram) {
            b.call(v.swi_vram, 0x02000000, 0x06000000);
            // Halfword stores: an odd final byte is never written.
            const std::size_t even = v.size & ~std::size_t{1};
            CHECK(b.peek(0x06000000, even) == std::vector<std::uint8_t>(expected.begin(), expected.begin() + static_cast<std::ptrdiff_t>(even)));
        }
        // The same data from ROM, the usual case.
        std::vector<std::uint8_t> rom(0x400, 0);
        rom.insert(rom.end(), packed.begin(), packed.end());
        System from_rom(rom, nullptr, quiet());
        from_rom.cpu().r[0] = 0x08000400;
        from_rom.cpu().r[1] = 0x02000000;
        from_rom.swi(v.swi_wram);
        std::vector<std::uint8_t> got(v.size);
        for (std::size_t i = 0; i < v.size; ++i) {
            got[i] = static_cast<std::uint8_t>(from_rom.read8(0x02000000 + static_cast<std::uint32_t>(i)));
        }
        CHECK(got == expected);
    }
}

TEST_CASE("BIOS decompressors refuse sources in the BIOS region") {
    Bios b;
    b.sys.write32(0x02000000, 0xDEADBEEF);
    b.call(0x11, 0x00000100, 0x02000000);
    CHECK(b.sys.read32(0x02000000) == 0xDEADBEEF);
}

TEST_CASE("BIOS Diff8/Diff16 unfilter") {
    Bios b;
    b.poke(0x02000000, {0x81, 0x0A, 0x00, 0x00, 10, 1, 1, 1, 0xFF, 2, 0, 0, 5, 5});  // 8-bit, 10 units
    b.call(0x16, 0x02000000, 0x03000000);
    CHECK(b.peek(0x03000000, 10) == std::vector<std::uint8_t>{10, 11, 12, 13, 12, 14, 14, 14, 19, 24});
    b.call(0x17, 0x02000000, 0x06000000);
    CHECK(b.peek(0x06000000, 10) == std::vector<std::uint8_t>{10, 11, 12, 13, 12, 14, 14, 14, 19, 24});
    b.poke(0x02000100, {0x82, 0x06, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0xFF, 0xFF});  // 16-bit, 3 units
    b.call(0x18, 0x02000100, 0x03000100);
    CHECK(b.peek(0x03000100, 6) == std::vector<std::uint8_t>{0x00, 0x10, 0x01, 0x10, 0x00, 0x10});
}

TEST_CASE("BIOS BitUnPack widens 1-bit pixels with an offset") {
    Bios b;
    b.poke(0x02000000, {0b10000001, 0b00000011});
    // info: 2 source bytes, 1 -> 4 bits, offset 2 added to non-zero units only
    b.poke(0x02000100, {2, 0, 1, 4, 2, 0, 0, 0});
    b.call(0x10, 0x02000000, 0x03000000, 0x02000100);
    CHECK(b.sys.read32(0x03000000) == 0x30000003u);
    CHECK(b.sys.read32(0x03000004) == 0x00000033u);
    // Zero-data flag: the offset is added to zero units as well.
    b.poke(0x02000100, {1, 0, 2, 8, 1, 0, 0, 0x80});
    b.poke(0x02000000, {0b11100100});
    b.call(0x10, 0x02000000, 0x03000010, 0x02000100);
    CHECK(b.sys.read32(0x03000010) == 0x04030201u);
}

TEST_CASE("BIOS CpuSet and CpuFastSet copy and fill") {
    Bios b;
    for (std::uint32_t i = 0; i < 64; ++i) {
        b.sys.write32(0x02000000 + 4 * i, 0x01010101u * i);
    }
    b.call(0x0B, 0x02000000, 0x03000000, 5 | (1u << 26));  // 5 words
    CHECK(b.sys.read32(0x03000010) == 0x04040404u);
    CHECK(b.sys.read32(0x03000014) == 0);
    b.call(0x0B, 0x02000004, 0x03000100, 3);  // 3 halfwords
    CHECK(b.sys.read16(0x03000104) == 0x0202);
    CHECK(b.sys.read16(0x03000106) == 0);
    b.call(0x0B, 0x02000008, 0x06000000, 4 | (1u << 24));  // 16-bit fill into VRAM
    CHECK(b.sys.read32(0x06000004) == 0x02020202u);
    b.call(0x0C, 0x02000000, 0x03000200, 9);  // rounded up to 16 words
    CHECK(b.sys.read32(0x0300023C) == 0x0F0F0F0Fu);
    CHECK(b.sys.read32(0x03000240) == 0);
    b.call(0x0C, 0x0200000C, 0x05000000, 8 | (1u << 24));  // fill palette
    CHECK(b.sys.read32(0x0500001C) == 0x03030303u);
    b.sys.write32(0x03000300, 0x12345678);
    b.call(0x0B, 0x00000000, 0x03000300, 1 | (1u << 26));  // source in the BIOS: ignored
    CHECK(b.sys.read32(0x03000300) == 0x12345678u);
}

TEST_CASE("BIOS RegisterRamReset clears what its flags select") {
    Bios b;
    b.sys.write32(0x02000000, 1);
    b.sys.write32(0x03000000, 2);
    b.sys.write32(0x03007F00, 3);  // last 0x200 bytes of IWRAM survive
    b.sys.write32(0x05000000, 4);
    b.sys.write32(0x06000000, 5);
    b.sys.write32(0x07000000, 6);
    b.sys.io_write16(0x008, 0x1234);  // BG0CNT
    b.sys.io_write16(0x080, 0x0077);  // SOUNDCNT_L
    b.call(0x01, 0x1F);
    CHECK(b.sys.read32(0x02000000) == 0);
    CHECK(b.sys.read32(0x03000000) == 0);
    CHECK(b.sys.read32(0x03007F00) == 3);
    CHECK(b.sys.read32(0x05000000) == 0);
    CHECK(b.sys.read32(0x06000000) == 0);
    CHECK(b.sys.read32(0x07000000) == 0);
    CHECK(b.sys.io_read16(0x008) == 0x1234);
    CHECK(b.sys.io_read16(0x000) == 0x0080);
    b.call(0x01, 0xC0);
    CHECK(b.sys.io_read16(0x008) == 0);
    CHECK(b.sys.io_read16(0x080) == 0);
    CHECK(b.sys.io_read16(0x020) == 0x100);  // BG2PA back to identity
    CHECK(b.sys.io_read16(0x088) == 0x200);
}

TEST_CASE("BIOS boot state and BIOS-region reads") {
    Bios b;
    CHECK(b.c.r[13] == 0x03007F00);
    CHECK(b.c.bank_r13_14[2][0] == 0x03007FA0);
    CHECK(b.c.bank_r13_14[3][0] == 0x03007FE0);
    CHECK((b.c.ctrl & 0x1F) == ANYGBA_MODE_SYS);
    CHECK(b.sys.io_read16(0x000) == 0x0080);
    CHECK(b.sys.io_read16(0x088) == 0x0200);
    CHECK(b.sys.io_read16(0x030) == 0x0100);
    CHECK(b.sys.read32(0x00000000) == anygba::bios::kOpcodeAfterBoot);
    b.call(0x0D);
    CHECK(b.c.r[0] == 0xBAAE187Fu);
    CHECK(b.sys.read32(0x00000100) == anygba::bios::kOpcodeAfterSwi);
}

TEST_CASE("BIOS SoftReset restarts the cartridge with fresh stacks") {
    // Counts boots in EWRAM (which SoftReset keeps); the first boot dirties registers and
    // the IRQ stack pointer, then calls SoftReset.
    System sys(rom_with({
                   {0x00, 0xE3A00402},  // mov r0, #0x02000000
                   {0x04, 0xE5901000},  // ldr r1, [r0]
                   {0x08, 0xE2811001},  // add r1, r1, #1
                   {0x0C, 0xE5801000},  // str r1, [r0]
                   {0x10, 0xE3510001},  // cmp r1, #1
                   {0x14, 0x1A000002},  // bne done
                   {0x18, 0xE3A0D000},  // mov sp, #0
                   {0x1C, 0xE3A05007},  // mov r5, #7
                   {0x20, 0xEF000000},  // swi 0
                   {0x24, 0xEAFFFFFE},  // done: b .
               }),
               nullptr, quiet());
    sys.on_idle_loop = [](System& s) { s.stop(); };
    sys.run();
    CHECK(sys.read32(0x02000000) == 2);
    CHECK(sys.cpu().r[15] == 0x08000024);
    CHECK(sys.cpu().r[13] == 0x03007F00);
    CHECK(sys.cpu().r[5] == 0);
}

TEST_CASE("BIOS rejects unknown SWIs and reports stubbed sound-driver calls") {
    Bios b;
    CHECK_THROWS_WITH_AS(b.call(0x2B), doctest::Contains("unknown BIOS call SWI 0x2B"), anygba::Error);
    b.c.r[0] = 0x1234;
    b.sys.swi(0x1C);  // SoundDriverMain: stub, logged once, registers untouched
    CHECK(b.c.r[0] == 0x1234);
    b.call(0x25);  // MultiBoot fails cleanly
    CHECK(b.c.r[0] == 1);
}

TEST_CASE("Keypad interrupt follows KEYCNT") {
    Bios b;
    b.sys.io_write16(0x132, 0x4000 | 0x0009);  // IRQ when A or Start
    b.sys.set_keys(0x0002);                    // B
    CHECK((b.sys.io_read16(0x202) & 0x1000) == 0);
    b.sys.set_keys(0x0008);  // Start
    CHECK((b.sys.io_read16(0x202) & 0x1000) != 0);
    b.sys.io_write16(0x202, 0x1000);
    b.sys.io_write16(0x132, 0xC000 | 0x0009);  // AND: both needed
    b.sys.set_keys(0x0008);
    CHECK((b.sys.io_read16(0x202) & 0x1000) == 0);
    b.sys.set_keys(0x0009);
    CHECK((b.sys.io_read16(0x202) & 0x1000) != 0);
}
