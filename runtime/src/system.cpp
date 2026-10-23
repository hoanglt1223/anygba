#include "anygba/system.hpp"
#include "apu/apu.h"
#include "ppu/ppu.h"

#include <algorithm>
#include <cstring>

namespace anygba {

System& system_of(anygba_cpu* c) { return *static_cast<System*>(c->runtime); }

System::System(std::vector<std::uint8_t> rom, const anygba_game* game, Options options)
    : options_(std::move(options)), game_(game), ppu_(std::make_unique<Ppu>(mem_)), apu_(std::make_unique<Apu>(*this)) {
    if (rom.size() < 0xC0 || rom.size() > Memory::kRomMax) {
        throw Error("ROM must be between 192 bytes and 32 MiB, got " + std::to_string(rom.size()) + " bytes");
    }
    mem_.rom_size = static_cast<std::uint32_t>(rom.size());
    mem_.rom = std::move(rom);
    mem_.rom.resize(Memory::kRomMax);
    // Reads past the end of the cartridge return the address bus: halfword (addr / 2).
    for (std::uint32_t off = (mem_.rom_size + 1) & ~1u; off < Memory::kRomMax; off += 2) {
        mem_.rom[off] = static_cast<std::uint8_t>(off >> 1);
        mem_.rom[off + 1] = static_cast<std::uint8_t>(off >> 9);
    }

    if (game_) {
        rom_direct_.assign(mem_.rom_size / 2 + 1, nullptr);
        for (std::uint32_t i = 0; i < game_->function_count; ++i) {
            const anygba_function_entry& e = game_->functions[i];
            if (e.addr >= 0x08000000 && e.addr - 0x08000000 < mem_.rom_size) {
                rom_direct_[(e.addr - 0x08000000) / 2] = &e;
            } else {
                ram_functions_.push_back(e);
            }
        }
        std::sort(ram_functions_.begin(), ram_functions_.end(),
                  [](const anygba_function_entry& a, const anygba_function_entry& b) { return a.addr < b.addr; });
    }
    reset();
}

void System::drain_audio(std::vector<std::int16_t>& out) {
    apu_->catch_up(now());
    apu_->drain(out);
}

std::uint64_t System::fifo_underruns() const { return apu_->fifo_underruns(); }

System::~System() {
    if (miss_file_) {
        std::fclose(miss_file_);
    }
}

void System::update_wait_states() {
    // Approximate per-instruction fetch cost. ROM waits come from WAITCNT; with the
    // prefetch buffer on, sequential fetches mostly hit the buffer.
    const std::uint16_t waitcnt = io_read16(0x204);
    static constexpr int kSeq[3][2] = {{2, 1}, {4, 1}, {8, 1}};  // WS0..2 S-cycle waits by bit
    const bool prefetch = (waitcnt & 0x4000) != 0;
    for (int t = 0; t < 2; ++t) {
        for (int r = 0; r < 16; ++r) {
            cpu_.fetch_cost[t][r] = 1;
        }
        cpu_.fetch_cost[t][0x2] = t ? 3 : 6;  // EWRAM: 16-bit bus, 2 waits
        cpu_.fetch_cost[t][0x5] = t ? 1 : 2;
        cpu_.fetch_cost[t][0x6] = t ? 1 : 2;
        for (int ws = 0; ws < 3; ++ws) {
            const int s = kSeq[ws][(waitcnt >> (4 + ws * 3)) & 1];
            const int halfword = prefetch ? 1 : 1 + s;
            cpu_.fetch_cost[t][0x8 + 2 * ws] = cpu_.fetch_cost[t][0x9 + 2 * ws] = t ? halfword : 2 * halfword;
        }
        cpu_.fetch_cost[t][0xE] = cpu_.fetch_cost[t][0xF] = 5;
    }
}

}  // namespace anygba
