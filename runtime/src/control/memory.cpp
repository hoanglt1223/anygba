#include "control/memory.h"

#include <cstdio>

namespace anygba::control {

namespace {

const std::uint8_t* byte_at(System& sys, std::uint32_t addr) {
    Memory& m = sys.memory();
    switch (addr >> 24) {
    case 0x00: return addr < Memory::kBiosSize ? &m.bios[addr] : nullptr;
    case 0x02: return &m.ewram[addr & (Memory::kEwramSize - 1)];
    case 0x03: return &m.iwram[addr & (Memory::kIwramSize - 1)];
    case 0x04: return addr < 0x04000000u + Memory::kIoSize ? &m.io[addr & 0x3FF] : nullptr;
    case 0x05: return &m.palette[addr & (Memory::kPaletteSize - 1)];
    case 0x06: {
        std::uint32_t off = addr & 0x1FFFF;
        if (off >= Memory::kVramSize) off -= 0x8000;
        return &m.vram[off];
    }
    case 0x07: return &m.oam[addr & (Memory::kOamSize - 1)];
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        return &m.rom[addr & (Memory::kRomMax - 1)];
    default: return nullptr;
    }
}

}  // namespace

void peek(System& sys, std::uint32_t addr, std::span<std::uint8_t> out) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint32_t a = addr + static_cast<std::uint32_t>(i);
        const std::uint8_t* p = byte_at(sys, a);
        if (!p) {
            char msg[64];
            std::snprintf(msg, sizeof msg, "0x%08X is not readable memory", a);
            throw Error(msg);
        }
        out[i] = *p;
    }
}

std::uint32_t peek_le(System& sys, std::uint32_t addr, int size) {
    std::uint8_t b[4] = {};
    peek(sys, addr, std::span(b, static_cast<std::size_t>(size)));
    return b[0] | (std::uint32_t{b[1]} << 8) | (std::uint32_t{b[2]} << 16) | (std::uint32_t{b[3]} << 24);
}

std::string region_name(std::uint32_t addr) {
    switch (addr >> 24) {
    case 0x00: return addr < Memory::kBiosSize ? "bios" : "unmapped";
    case 0x02: return "ewram";
    case 0x03: return "iwram";
    case 0x04: return addr < 0x04000000u + Memory::kIoSize ? "io" : "unmapped";
    case 0x05: return "palette";
    case 0x06: return "vram";
    case 0x07: return "oam";
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: return "rom";
    case 0x0E: case 0x0F: return "backup";
    default: return "unmapped";
    }
}

}  // namespace anygba::control
