#include "anygba/system.hpp"

#include <doctest.h>

#include <cstdint>
#include <vector>

using anygba::System;

namespace {

// Hand-assembled ARM programs; each word is commented with its assembly.
std::vector<std::uint8_t> rom_with(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> words) {
    std::vector<std::uint8_t> rom(0x400, 0);
    for (const auto& [off, w] : words) {
        for (int b = 0; b < 4; ++b) {
            rom.at(off + static_cast<std::uint32_t>(b)) = static_cast<std::uint8_t>(w >> (8 * b));
        }
    }
    return rom;
}

std::vector<std::uint8_t> idle_rom() { return rom_with({{0x00, 0xEAFFFFFE}}); }  // b .

anygba::Options quiet() {
    anygba::Options o;
    o.log_misses = false;
    return o;
}

void run_frames(System& sys, std::uint64_t frames) {
    sys.on_vblank = [frames](System& s) {
        if (s.frames() >= frames) {
            s.stop();
        }
    };
    sys.run();
}

// Sets the IRQ vector, enables the VBlank interrupt, then runs `main_loop` (offset 0x28..).
std::vector<std::uint8_t> irq_rom(std::uint32_t handler, std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> extra) {
    std::vector<std::uint8_t> rom = rom_with({
        {0x00, 0xE59F0038},  // ldr r0, [pc, #0x38]     (=0x03007ffc)
        {0x04, 0xE59F1038},  // ldr r1, [pc, #0x38]     (=handler)
        {0x08, 0xE5801000},  // str r1, [r0]
        {0x0C, 0xE3A00301},  // mov r0, #0x04000000
        {0x10, 0xE2802C02},  // add r2, r0, #0x200
        {0x14, 0xE3A01001},  // mov r1, #1
        {0x18, 0xE1C210B0},  // strh r1, [r2]           IE = VBlank
        {0x1C, 0xE5821008},  // str r1, [r2, #8]        IME = 1
        {0x20, 0xE3A01008},  // mov r1, #8
        {0x24, 0xE1C010B4},  // strh r1, [r0, #4]       DISPSTAT: VBlank IRQ enable
        {0x28, 0xEAFFFFFE},  // b .
        {0x40, 0x03007FFC},
        {0x44, handler},
        // handler A: count IRQs at 0x03000000, acknowledge IF
        {0x100, 0xE59F1018},  // ldr r1, [pc, #0x18]    (=0x03000000)
        {0x104, 0xE5912000},  // ldr r2, [r1]
        {0x108, 0xE2822001},  // add r2, r2, #1
        {0x10C, 0xE5812000},  // str r2, [r1]
        {0x110, 0xE2803C02},  // add r3, r0, #0x200
        {0x114, 0xE3A02001},  // mov r2, #1
        {0x118, 0xE1C320B2},  // strh r2, [r3, #2]      IF = VBlank (acknowledge)
        {0x11C, 0xE12FFF1E},  // bx lr
        {0x120, 0x03000000},
        // handler B: acknowledge IF and flag VBlank in the BIOS IF copy at 0x03007ff8
        {0x140, 0xE59F1020},  // ldr r1, [pc, #0x20]    (=0x03007ff8)
        {0x144, 0xE1D120B0},  // ldrh r2, [r1]
        {0x148, 0xE3822001},  // orr r2, r2, #1
        {0x14C, 0xE1C120B0},  // strh r2, [r1]
        {0x150, 0xE2803C02},  // add r3, r0, #0x200
        {0x154, 0xE3A02001},  // mov r2, #1
        {0x158, 0xE1C320B2},  // strh r2, [r3, #2]
        {0x15C, 0xE12FFF1E},  // bx lr
        {0x168, 0x03007FF8},
    });
    for (const auto& [off, w] : extra) {
        for (int b = 0; b < 4; ++b) {
            rom.at(off + static_cast<std::uint32_t>(b)) = static_cast<std::uint8_t>(w >> (8 * b));
        }
    }
    return rom;
}

}  // namespace

TEST_CASE("video timing: 1232 cycles per line, 228 lines, VBlank at line 160") {
    System sys(idle_rom(), nullptr, quiet());
    std::vector<std::uint64_t> at;
    std::vector<std::uint16_t> dispstat;
    sys.on_vblank = [&](System& s) {
        at.push_back(s.now());
        dispstat.push_back(s.io_read16(0x004));
        if (s.frames() >= 3) {
            s.stop();
        }
    };
    sys.run();
    REQUIRE(at.size() == 3);
    CHECK(at[0] == 160u * 1232u);
    CHECK(at[1] - at[0] == 280896u);
    CHECK(at[2] - at[1] == 280896u);
    CHECK((dispstat[0] & 1) == 1);  // VBlank flag
    CHECK((dispstat[0] & 2) == 0);  // HBlank ends with the line
    CHECK(sys.io_read16(0x006) == 160);
}

TEST_CASE("timers: prescaler, overflow interrupt flag, and count-up cascade") {
    System sys(idle_rom(), nullptr, quiet());
    sys.io_write16(0x100, 0xFF00);  // TM0 reload: overflow every 256 ticks
    sys.io_write16(0x102, 0x00C0);  // enable, IRQ, prescaler 1
    sys.io_write16(0x104, 0xFFFE);  // TM1 reload
    sys.io_write16(0x106, 0x00C4);  // enable, IRQ, count-up
    CHECK(sys.io_read16(0x100) == 0xFF00);
    run_frames(sys, 1);
    const std::uint16_t iflag = sys.io_read16(0x202);
    CHECK((iflag & (1u << 3)) != 0);  // timer 0 overflowed
    CHECK((iflag & (1u << 4)) != 0);  // timer 1 overflowed after two timer 0 overflows
    // Counter after the run: reload plus elapsed cycles since the last overflow.
    const std::uint64_t ticks = sys.now() % 256;
    CHECK(sys.io_read16(0x100) == 0xFF00 + ticks);

    sys.io_write16(0x10A, 0x0083);  // TM2: enable, prescaler 1024
    const std::uint64_t start = sys.now();
    run_frames(sys, 2);
    CHECK(sys.io_read16(0x108) == static_cast<std::uint16_t>((sys.now() - start) >> 10));
}

TEST_CASE("DMA: immediate transfers with address control, IRQ on completion") {
    System sys(idle_rom(), nullptr, quiet());
    for (std::uint32_t i = 0; i < 8; ++i) {
        sys.write16(0x02000000 + 2 * i, 0x1000 + i);
    }
    // DMA3: 4 halfwords from EWRAM (decrementing) to IWRAM (incrementing), IRQ.
    sys.write32(0x040000D4, 0x02000006);
    sys.write32(0x040000D8, 0x03000000);
    sys.write16(0x040000DC, 4);
    sys.write16(0x040000DE, 0x8000 | 0x4000 | (1u << 7));
    CHECK(sys.read16(0x03000000) == 0x1003);
    CHECK(sys.read16(0x03000006) == 0x1000);
    CHECK((sys.io_read16(0x0DE) & 0x8000) == 0);  // a non-repeating transfer disables itself
    CHECK((sys.io_read16(0x202) & (1u << 11)) != 0);

    // DMA0: 32-bit, fixed destination: the last word wins.
    sys.write32(0x040000B0, 0x02000000);
    sys.write32(0x040000B4, 0x03000100);
    sys.write16(0x040000B8, 4);
    sys.write16(0x040000BA, 0x8000 | 0x0400 | (2u << 5));
    CHECK(sys.read32(0x03000100) == sys.read32(0x0200000C));
}

TEST_CASE("DMA: HBlank repeat runs once per visible line and reloads the destination") {
    System sys(idle_rom(), nullptr, quiet());
    for (std::uint32_t i = 0; i < 1024; ++i) {
        sys.write16(0x02000000 + 2 * i, static_cast<std::uint16_t>(i));
    }
    sys.write32(0x040000C8, 0x02000000);  // DMA2 source
    sys.write32(0x040000CC, 0x03000000);  // destination, reloaded each time
    sys.write16(0x040000D0, 1);
    sys.write16(0x040000D2, 0x8000 | 0x0200 | (2u << 12) | (3u << 5));
    run_frames(sys, 1);
    // HBlanks of lines 0-159 ran before the first VBlank; the source walked 160 halfwords.
    CHECK(sys.read16(0x03000000) == 159);
    CHECK((sys.io_read16(0x0D2) & 0x8000) != 0);
}

TEST_CASE("sound FIFO DMA moves four words per request") {
    System sys(idle_rom(), nullptr, quiet());
    for (std::uint32_t i = 0; i < 8; ++i) {
        sys.write32(0x02000000 + 4 * i, 0xA0000000 + i);
    }
    sys.write32(0x040000BC, 0x02000000);  // DMA1 source
    sys.write32(0x040000C0, 0x040000A0);  // FIFO A
    sys.write16(0x040000C4, 4);
    sys.write16(0x040000C6, 0x8000 | 0x0200 | 0x0400 | (3u << 12) | (2u << 5));
    // FIFO_A is write-only; the raw IO bytes keep the last word the DMA wrote.
    const auto last_word = [&sys] { return sys.memory().io[0x0A0] | (sys.memory().io[0x0A1] << 8); };
    sys.request_fifo_dma(1);  // FIFO B: no channel targets it
    CHECK(last_word() == 0);
    sys.request_fifo_dma(0);
    CHECK(last_word() == 0x0003);
    sys.request_fifo_dma(0);
    CHECK(last_word() == 0x0007);
}

TEST_CASE("IRQs run the handler at 0x03007FFC once per VBlank") {
    System sys(irq_rom(0x08000100, {}), nullptr, quiet());
    run_frames(sys, 10);
    CHECK(sys.read32(0x03000000) == 10);
    CHECK((sys.cpu().ctrl & 0x1F) == ANYGBA_MODE_SYS);  // back in the interrupted mode
    CHECK(sys.cpu().r[13] == 0x03007F00);
}

TEST_CASE("VBlankIntrWait sleeps until the handler flags VBlank") {
    System sys(irq_rom(0x08000140, {
                                       {0x28, 0xEF050000},  // swi 0x05     VBlankIntrWait
                                       {0x2C, 0xE59F4014},  // ldr r4, [pc, #0x14]  (=0x03000004)
                                       {0x30, 0xE5945000},  // ldr r5, [r4]
                                       {0x34, 0xE2855001},  // add r5, r5, #1
                                       {0x38, 0xE5845000},  // str r5, [r4]
                                       {0x3C, 0xEAFFFFF9},  // b 0x08000028
                                       {0x48, 0x03000004},
                                   }),
               nullptr, quiet());
    run_frames(sys, 20);
    // One pass of the loop per frame; the last VBlank stops the run before the loop sees it.
    const std::uint32_t loops = sys.read32(0x03000004);
    CHECK(loops >= 19);
    CHECK(loops <= 20);
}

TEST_CASE("bus quirks: byte writes to video memory, SRAM bus width, ROM open bus") {
    std::vector<std::uint8_t> rom(0x1000, 0);
    anygba::Options sram = quiet();
    sram.save_type = "sram";
    System sys(rom, nullptr, sram);
    sys.write8(0x05000011, 0xAB);
    CHECK(sys.read16(0x05000010) == 0xABAB);  // palette: byte doubled
    sys.write8(0x07000010, 0xAB);
    CHECK(sys.read16(0x07000010) == 0);  // OAM ignores bytes
    sys.write8(0x06014000, 0xCD);
    CHECK(sys.read16(0x06014000) == 0);  // OBJ VRAM ignores bytes in tile modes
    sys.write8(0x06000001, 0xCD);
    CHECK(sys.read16(0x06000000) == 0xCDCD);
    CHECK(sys.read16(0x06018000) == sys.read16(0x06010000));  // 32 KiB mirror
    sys.write8(0x0E000005, 0x5A);
    CHECK(sys.read16(0x0E000005) == 0x5A5A);
    CHECK(sys.read32(0x0E000005) == 0x5A5A5A5A);
    CHECK(sys.read16(0x08001000) == 0x0800);  // past the image: halfword address / 2
    CHECK(sys.read16(0x0A001002) == 0x0801);
    CHECK(sys.read32(0x0200FFF0) == sys.read32(0x0204FFF0));  // EWRAM mirror
}
