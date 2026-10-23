#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace anygba::analysis {

inline constexpr std::uint32_t kRomBase = 0x08000000;
inline constexpr std::uint32_t kRomMaxSize = 0x02000000;

enum class Mode : std::uint8_t { Arm, Thumb };

// Code copied from ROM into RAM at run time: `size` bytes from `rom_src` appear at `ram_dst`.
struct RamRegion {
    std::uint32_t rom_src = 0;
    std::uint32_t ram_dst = 0;
    std::uint32_t size = 0;
};

// Read-only view of everything the recompiler can see: the ROM at 0x08000000 (and its
// wait-state mirrors at 0x0A000000 / 0x0C000000), plus RAM regions backed by ROM bytes.
class Image {
public:
    explicit Image(std::vector<std::uint8_t> rom);

    void add_region(const RamRegion& region);
    const std::vector<RamRegion>& regions() const { return regions_; }
    std::span<const std::uint8_t> rom() const { return rom_; }

    // ROM offset backing `addr`, if `size` bytes there are backed by the ROM.
    std::optional<std::uint32_t> rom_offset(std::uint32_t addr, std::uint32_t size) const;
    bool contains(std::uint32_t addr, std::uint32_t size) const { return rom_offset(addr, size).has_value(); }

    std::optional<std::uint16_t> read16(std::uint32_t addr) const;
    std::optional<std::uint32_t> read32(std::uint32_t addr) const;

    void patch(std::uint32_t addr, std::span<const std::uint8_t> bytes);

    // Canonical ROM address (0x08xxxxxx) of a ROM mirror address; other addresses unchanged.
    static std::uint32_t canonical(std::uint32_t addr);
    static bool is_rom(std::uint32_t addr) { return addr >= kRomBase && addr < 0x0E000000; }

private:
    std::vector<std::uint8_t> rom_;
    std::vector<RamRegion> regions_;
};

}  // namespace anygba::analysis
