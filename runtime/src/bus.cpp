#include "anygba/system.hpp"
#include "save/save.h"

#include <bit>
#include <cstring>

// Memory map per GBATEK "GBA Memory Map". The fast path in cpu.h covers EWRAM, IWRAM and
// ROM reads and EWRAM/IWRAM writes; everything else, and every access a page table does
// not cover, lands here.
namespace anygba {

namespace {

std::uint32_t vram_offset(std::uint32_t addr) {
    std::uint32_t off = addr & 0x1FFFF;
    if (off >= 0x18000) {
        off -= 0x8000;  // 0x18000-0x1FFFF mirrors the OBJ area at 0x10000-0x17FFF
    }
    return off;
}

std::uint32_t load(const std::vector<std::uint8_t>& mem, std::uint32_t off, int size) {
    std::uint32_t v = 0;
    std::memcpy(&v, mem.data() + off, static_cast<std::size_t>(size));
    return v;
}

void store(std::vector<std::uint8_t>& mem, std::uint32_t off, std::uint32_t v, int size) {
    std::memcpy(mem.data() + off, &v, static_cast<std::size_t>(size));
}

}  // namespace

void System::map_pages() {
    for (int i = 0; i < 256; ++i) {
        cpu_.read_page[i] = nullptr;
        cpu_.write_page[i] = nullptr;
        cpu_.read_mask[i] = 0;
        cpu_.write_mask[i] = 0;
    }
    cpu_.read_page[0x02] = cpu_.write_page[0x02] = mem_.ewram.data();
    cpu_.read_mask[0x02] = cpu_.write_mask[0x02] = Memory::kEwramSize - 1;
    cpu_.read_page[0x03] = cpu_.write_page[0x03] = mem_.iwram.data();
    cpu_.read_mask[0x03] = cpu_.write_mask[0x03] = Memory::kIwramSize - 1;
    // With an EEPROM, 0x0D reads may hit the serial port, so they take the slow path.
    for (int i = 0x08; i <= (eeprom_ ? 0x0C : 0x0D); ++i) {
        cpu_.read_page[i] = mem_.rom.data();
        cpu_.read_mask[i] = Memory::kRomMax - 1;
    }
    for (int region = 0; region < 16; ++region) {
        if ((watched_regions_ >> region) & 1u) cpu_.read_page[region] = cpu_.write_page[region] = nullptr;
    }
}

void System::set_watched_regions(std::uint16_t mask) {
    watched_regions_ = mask;
    map_pages();
}

std::uint32_t System::open_bus() const {
    // The real value is the last prefetched opcode, which recompiled code does not track.
    return 0;
}

// The EEPROM answers in the whole 0x0D area on carts up to 16 MB, and only at the top
// (0x0DFFFF00+) on 32 MB carts, where the ROM needs the rest of the area.
bool System::eeprom_at(std::uint32_t addr) const {
    return eeprom_ && (addr >> 24) == 0x0D && (mem_.rom_size <= 0x1000000 || addr >= 0x0DFFFF00);
}

std::uint16_t System::eeprom_read() { return backup_->eeprom_read(); }

void System::eeprom_write(std::uint32_t value) {
    std::string error;
    try {
        backup_->eeprom_write(static_cast<std::uint16_t>(value), eeprom_dma_count_);
        return;
    } catch (const std::exception& e) {
        error = e.what();
    }
    fail(std::move(error));  // longjmp only after the exception object is gone
}

std::uint32_t System::bus_read32(std::uint32_t addr) {
    const std::uint32_t unaligned = addr;  // SRAM sits on an 8-bit bus and reads the exact byte
    addr &= ~3u;
    switch (addr >> 24) {
    case 0x00:
        return addr < Memory::kBiosSize ? last_bios_opcode_ : open_bus();
    case 0x02: return load(mem_.ewram, addr & (Memory::kEwramSize - 1), 4);
    case 0x03: return load(mem_.iwram, addr & (Memory::kIwramSize - 1), 4);
    case 0x04:
        if (addr < 0x04000000 + Memory::kIoSize) {
            return io_read16(addr & 0x3FF) | (static_cast<std::uint32_t>(io_read16((addr & 0x3FF) + 2)) << 16);
        }
        return open_bus();
    case 0x05: return load(mem_.palette, addr & (Memory::kPaletteSize - 1), 4);
    case 0x06: return load(mem_.vram, vram_offset(addr), 4);
    case 0x07: return load(mem_.oam, addr & (Memory::kOamSize - 1), 4);
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        return load(mem_.rom, addr & (Memory::kRomMax - 1), 4);
    case 0x0E: case 0x0F:
        return read8(unaligned) * 0x01010101u;
    default:
        return open_bus();
    }
}

std::uint32_t System::bus_read16(std::uint32_t addr) {
    const std::uint32_t unaligned = addr;
    addr &= ~1u;
    switch (addr >> 24) {
    case 0x00:
        return addr < Memory::kBiosSize ? (last_bios_opcode_ >> ((addr & 2) * 8)) & 0xFFFF : open_bus() & 0xFFFF;
    case 0x02: return load(mem_.ewram, addr & (Memory::kEwramSize - 1), 2);
    case 0x03: return load(mem_.iwram, addr & (Memory::kIwramSize - 1), 2);
    case 0x04:
        return addr < 0x04000000 + Memory::kIoSize ? io_read16(addr & 0x3FF) : open_bus() & 0xFFFF;
    case 0x05: return load(mem_.palette, addr & (Memory::kPaletteSize - 1), 2);
    case 0x06: return load(mem_.vram, vram_offset(addr), 2);
    case 0x07: return load(mem_.oam, addr & (Memory::kOamSize - 1), 2);
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        if (eeprom_at(addr)) {
            return eeprom_read();
        }
        return load(mem_.rom, addr & (Memory::kRomMax - 1), 2);
    case 0x0E: case 0x0F:
        return read8(unaligned) * 0x0101u;
    default:
        return open_bus() & 0xFFFF;
    }
}

std::uint32_t System::bus_read8(std::uint32_t addr) {
    switch (addr >> 24) {
    case 0x00:
        return addr < Memory::kBiosSize ? (last_bios_opcode_ >> ((addr & 3) * 8)) & 0xFF : open_bus() & 0xFF;
    case 0x02: return mem_.ewram[addr & (Memory::kEwramSize - 1)];
    case 0x03: return mem_.iwram[addr & (Memory::kIwramSize - 1)];
    case 0x04:
        return addr < 0x04000000 + Memory::kIoSize ? (io_read16(addr & 0x3FE) >> ((addr & 1) * 8)) & 0xFF
                                                    : open_bus() & 0xFF;
    case 0x05: return mem_.palette[addr & (Memory::kPaletteSize - 1)];
    case 0x06: return mem_.vram[vram_offset(addr)];
    case 0x07: return mem_.oam[addr & (Memory::kOamSize - 1)];
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        return mem_.rom[addr & (Memory::kRomMax - 1)];
    case 0x0E: case 0x0F:
        return backup_->read8(addr);
    default:
        return open_bus() & 0xFF;
    }
}

void System::bus_write32(std::uint32_t addr, std::uint32_t value) {
    const std::uint32_t unaligned = addr;
    addr &= ~3u;
    switch (addr >> 24) {
    case 0x02: store(mem_.ewram, addr & (Memory::kEwramSize - 1), value, 4); break;
    case 0x03: store(mem_.iwram, addr & (Memory::kIwramSize - 1), value, 4); break;
    case 0x04:
        if (addr < 0x04000000 + Memory::kIoSize) {
            io_write16(addr & 0x3FF, static_cast<std::uint16_t>(value));
            io_write16((addr & 0x3FF) + 2, static_cast<std::uint16_t>(value >> 16));
        }
        break;
    case 0x05: store(mem_.palette, addr & (Memory::kPaletteSize - 1), value, 4); break;
    case 0x06: store(mem_.vram, vram_offset(addr), value, 4); ++vram_writes_; break;
    case 0x07: store(mem_.oam, addr & (Memory::kOamSize - 1), value, 4); break;
    case 0x08:
        gpio_write(addr, static_cast<std::uint16_t>(value));
        gpio_write(addr + 2, static_cast<std::uint16_t>(value >> 16));
        break;
    case 0x0E: case 0x0F:
        // 8-bit bus: the byte lane the address selects is stored.
        write8(unaligned, std::rotr(value, static_cast<int>((unaligned & 3) * 8)));
        break;
    default: break;  // BIOS and ROM are read-only; unmapped writes vanish
    }
}

void System::bus_write16(std::uint32_t addr, std::uint32_t value) {
    const std::uint32_t unaligned = addr;
    addr &= ~1u;
    switch (addr >> 24) {
    case 0x02: store(mem_.ewram, addr & (Memory::kEwramSize - 1), value, 2); break;
    case 0x03: store(mem_.iwram, addr & (Memory::kIwramSize - 1), value, 2); break;
    case 0x04:
        if (addr < 0x04000000 + Memory::kIoSize) {
            io_write16(addr & 0x3FF, static_cast<std::uint16_t>(value));
        }
        break;
    case 0x05: store(mem_.palette, addr & (Memory::kPaletteSize - 1), value, 2); break;
    case 0x06: store(mem_.vram, vram_offset(addr), value, 2); ++vram_writes_; break;
    case 0x07: store(mem_.oam, addr & (Memory::kOamSize - 1), value, 2); break;
    case 0x08:
        gpio_write(addr, static_cast<std::uint16_t>(value));
        break;
    case 0x0D:
        if (eeprom_at(addr)) {
            eeprom_write(value);
        }
        break;
    case 0x0E: case 0x0F:
        write8(unaligned, (value >> ((unaligned & 1) * 8)) & 0xFF);
        break;
    default: break;
    }
}

void System::bus_write8(std::uint32_t addr, std::uint32_t value) {
    const auto byte = static_cast<std::uint8_t>(value);
    switch (addr >> 24) {
    case 0x02: mem_.ewram[addr & (Memory::kEwramSize - 1)] = byte; break;
    case 0x03: mem_.iwram[addr & (Memory::kIwramSize - 1)] = byte; break;
    case 0x04:
        if (addr < 0x04000000 + Memory::kIoSize) {
            const std::uint32_t off = addr & 0x3FE;
            const int shift = (addr & 1) ? 8 : 0;
            if (off == 0x202) {
                io_write16(off, static_cast<std::uint16_t>(byte << shift));  // IF: write 1 to clear
                break;
            }
            if (off == 0x300 && (addr & 1)) {
                io_write16(off, static_cast<std::uint16_t>((mem_.io[0x300]) | (byte << 8)));  // HALTCNT
                break;
            }
            // Merge with the stored value, not the read value: write-only registers read as 0.
            std::uint16_t old;
            std::memcpy(&old, mem_.io.data() + off, 2);
            const std::uint16_t merged = static_cast<std::uint16_t>((old & ~(0xFF << shift)) | (byte << shift));
            io_write16(off, merged);
        }
        break;
    case 0x05: {
        // Byte writes to palette RAM store the byte in both halves of the halfword.
        const std::uint32_t off = addr & (Memory::kPaletteSize - 2);
        mem_.palette[off] = mem_.palette[off + 1] = byte;
        break;
    }
    case 0x06: {
        // Byte writes reach BG VRAM only (and are doubled); OBJ VRAM ignores them.
        const std::uint32_t off = vram_offset(addr) & ~1u;
        const bool bitmap = (io_read16(0) & 7) >= 3;
        if (off < (bitmap ? 0x14000u : 0x10000u)) {
            mem_.vram[off] = mem_.vram[off + 1] = byte;
            ++vram_writes_;
        }
        break;
    }
    case 0x07: break;  // OAM ignores byte writes
    case 0x0E: case 0x0F: backup_->write8(addr, byte); break;
    default: break;
    }
}

}  // namespace anygba

extern "C" {

std::uint32_t anygba_bus_read32(anygba_cpu* c, std::uint32_t addr) { return anygba::system_of(c).read32(addr); }
std::uint32_t anygba_bus_read16(anygba_cpu* c, std::uint32_t addr) { return anygba::system_of(c).read16(addr); }
std::uint32_t anygba_bus_read8(anygba_cpu* c, std::uint32_t addr) { return anygba::system_of(c).read8(addr); }
void anygba_bus_write32(anygba_cpu* c, std::uint32_t addr, std::uint32_t v) { anygba::system_of(c).write32(addr, v); }
void anygba_bus_write16(anygba_cpu* c, std::uint32_t addr, std::uint32_t v) { anygba::system_of(c).write16(addr, v); }
void anygba_bus_write8(anygba_cpu* c, std::uint32_t addr, std::uint32_t v) { anygba::system_of(c).write8(addr, v); }

}
