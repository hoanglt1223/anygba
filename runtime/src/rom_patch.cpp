#include "anygba/system.hpp"

#include <cstring>

// ROM patch cheats (docs/dev/CHEATS.md). The patched halfwords are written into the ROM
// image, which the interpreter and data reads see directly. Recompiled code has the old
// instructions and folded literals built in, so every generated function whose code range
// contains a patched address is bypassed: lookup() stops returning it and its AG_ENTER
// check sends direct calls to the dispatcher, which interprets the patched code.
namespace anygba {

namespace {

std::uint32_t canonical(std::uint32_t addr) { return 0x08000000u + (addr & (Memory::kRomMax - 2)); }

}  // namespace

void System::set_rom_patches(const std::map<std::uint32_t, std::uint16_t>& patches) {
    rom_sha256();  // the state-file identity is the unpatched image
    for (const auto& [addr, original] : rom_originals_) {
        std::memcpy(mem_.rom.data() + (addr - 0x08000000u), &original, 2);
    }
    rom_originals_.clear();
    rom_patches_.clear();
    for (const auto& [raw_addr, value] : patches) {
        if (raw_addr < 0x08000000 || raw_addr >= 0x0E000000) {
            throw Error("ROM patch address outside the cartridge: " + std::to_string(raw_addr));
        }
        const std::uint32_t addr = canonical(raw_addr);
        std::uint16_t original;
        std::memcpy(&original, mem_.rom.data() + (addr - 0x08000000u), 2);
        rom_originals_.emplace(addr, original);
        std::memcpy(mem_.rom.data() + (addr - 0x08000000u), &value, 2);
        rom_patches_[addr] = value;
    }

    bypassed_.clear();
    if (game_) {
        for (std::uint32_t i = 0; i < game_->function_count; ++i) {
            const anygba_function_entry& e = game_->functions[i];
            const auto it = rom_patches_.lower_bound(e.code_begin & ~1u);
            if (e.code_end > e.code_begin && it != rom_patches_.end() && it->first < e.code_end) {
                bypassed_.insert(e.addr);
            }
        }
    }
    cpu_.rom_patched = rom_patches_.empty() && breakpoints_.empty() ? 0 : 1;
}

// Breakpoints share the AG_ENTER check: while any are set, every generated ROM function asks.
void System::set_breakpoints(std::set<std::uint32_t> addrs) {
    breakpoints_ = std::move(addrs);
    cpu_.rom_patched = rom_patches_.empty() && breakpoints_.empty() ? 0 : 1;
}

}  // namespace anygba

extern "C" int anygba_rom_patched(anygba_cpu* c, std::uint32_t entry) {
    anygba::System& sys = anygba::system_of(c);
    sys.hit_breakpoint(entry);
    return sys.rom_bypassed(entry) ? 1 : 0;
}

extern "C" int anygba_ram_code_valid(anygba_cpu* c, std::uint32_t entry, int thumb) {
    return anygba::system_of(c).ram_code_valid(entry, thumb != 0) ? 1 : 0;
}
