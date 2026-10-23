#include "anygba/system.hpp"

#include <algorithm>
#include <cstring>

namespace anygba {

System& system_of(anygba_cpu* c) { return *static_cast<System*>(c->runtime); }

System::System(std::vector<std::uint8_t> rom, const anygba_game* game, Options options)
    : options_(std::move(options)), game_(game) {
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

System::~System() {
    if (miss_file_) {
        std::fclose(miss_file_);
    }
}

void System::reset() {
    void* const runtime = this;
    std::memset(&cpu_, 0, sizeof cpu_);
    cpu_.runtime = runtime;
    map_pages();

    std::fill(mem_.ewram.begin(), mem_.ewram.end(), 0);
    std::fill(mem_.iwram.begin(), mem_.iwram.end(), 0);
    std::fill(mem_.io.begin(), mem_.io.end(), 0);
    std::fill(mem_.palette.begin(), mem_.palette.end(), 0);
    std::fill(mem_.vram.begin(), mem_.vram.end(), 0);
    std::fill(mem_.oam.begin(), mem_.oam.end(), 0);

    // Stacks and mode as the BIOS leaves them before entering the cartridge.
    cpu_.bank_r13_14[0][0] = 0x03007F00;  // USR/SYS
    cpu_.bank_r13_14[2][0] = 0x03007FA0;  // IRQ
    cpu_.bank_r13_14[3][0] = 0x03007FE0;  // SVC
    cpu_.ctrl = ANYGBA_MODE_SYS;
    cpu_.r[13] = 0x03007F00;
    cpu_.r[15] = 0x08000000;
    cpu_.t = 0;

    dma_ = {};
    timers_ = {};
    io_write16(0x000, 0x0080);  // forced blank, as the BIOS leaves DISPCNT
    set_keys(0);
    update_wait_states();

    line_ = 0;
    hblank_ = false;
    frames_ = 0;
    halted_ = false;
    in_irq_ = false;
    last_bios_opcode_ = 0xE129F000;
    budget_start_ = 0;
    budget_ = 0;
    events_.fill(UINT64_MAX);
    schedule(Event::HBlank, 1006);
    schedule(Event::LineEnd, 1232);
    reschedule_budget();
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
