#include "image.h"

#include <stdexcept>
#include <string>

namespace anygba::analysis {

Image::Image(std::vector<std::uint8_t> rom) : rom_(std::move(rom)) {
    if (rom_.empty() || rom_.size() > kRomMaxSize) {
        throw std::invalid_argument("ROM size must be between 1 byte and 32 MiB, got " + std::to_string(rom_.size()));
    }
}

void Image::add_region(const RamRegion& region) {
    const bool in_ram = (region.ram_dst >= 0x02000000 && region.ram_dst + region.size <= 0x02040000) ||
                        (region.ram_dst >= 0x03000000 && region.ram_dst + region.size <= 0x03008000);
    const auto src = static_cast<std::uint64_t>(region.rom_src);
    if (!in_ram || region.size == 0 || src < kRomBase || src + region.size > kRomBase + rom_.size()) {
        throw std::invalid_argument("RAM region must copy ROM bytes into EWRAM or IWRAM");
    }
    regions_.push_back(region);
}

std::uint32_t Image::canonical(std::uint32_t addr) {
    return is_rom(addr) ? kRomBase + (addr & (kRomMaxSize - 1)) : addr;
}

std::optional<std::uint32_t> Image::rom_offset(std::uint32_t addr, std::uint32_t size) const {
    std::uint64_t offset;
    if (is_rom(addr)) {
        offset = addr & (kRomMaxSize - 1);
    } else {
        // The most recently added region wins where regions overlap.
        const RamRegion* hit = nullptr;
        for (const auto& r : regions_) {
            if (addr >= r.ram_dst && static_cast<std::uint64_t>(addr) + size <= static_cast<std::uint64_t>(r.ram_dst) + r.size) {
                hit = &r;
            }
        }
        if (!hit) {
            return std::nullopt;
        }
        offset = static_cast<std::uint64_t>(hit->rom_src - kRomBase) + (addr - hit->ram_dst);
    }
    if (offset + size > rom_.size()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(offset);
}

std::optional<std::uint16_t> Image::read16(std::uint32_t addr) const {
    const auto off = rom_offset(addr, 2);
    if (!off) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(rom_[*off] | (rom_[*off + 1] << 8));
}

std::optional<std::uint32_t> Image::read32(std::uint32_t addr) const {
    const auto off = rom_offset(addr, 4);
    if (!off) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(rom_[*off]) | (static_cast<std::uint32_t>(rom_[*off + 1]) << 8) |
           (static_cast<std::uint32_t>(rom_[*off + 2]) << 16) | (static_cast<std::uint32_t>(rom_[*off + 3]) << 24);
}

void Image::patch(std::uint32_t addr, std::span<const std::uint8_t> bytes) {
    const auto off = rom_offset(addr, static_cast<std::uint32_t>(bytes.size()));
    if (!off) {
        throw std::invalid_argument("patch outside the ROM image");
    }
    std::copy(bytes.begin(), bytes.end(), rom_.begin() + *off);
}

}  // namespace anygba::analysis
