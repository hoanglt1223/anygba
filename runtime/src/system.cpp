#include "anygba/system.hpp"
#include "apu/apu.h"
#include "ppu/ppu.h"
#include "rtc.h"
#include "save/save.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <string_view>

namespace anygba {

System& system_of(anygba_cpu* c) { return *static_cast<System*>(c->runtime); }

namespace {

// Games that shipped on cartridges with the S-3511 clock, by the first three letters of the
// game code (the fourth is the region).
bool rtc_cartridge(std::span<const std::uint8_t> rom) {
    static constexpr std::string_view kCodes[] = {
        "AXV", "AXP", "BPE",  // Pokemon Ruby, Sapphire, Emerald
        "U3I", "U32", "U33",  // Boktai 1-3
        "BR4",                // Rockman EXE 4.5
        "BKA",                // Sennen Kazoku
    };
    const std::string_view code(reinterpret_cast<const char*>(rom.data()) + 0xAC, 3);
    return std::find(std::begin(kCodes), std::end(kCodes), code) != std::end(kCodes);
}

Rtc::Clock to_clock(const std::tm& tm) {
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_wday, tm.tm_hour, tm.tm_min, tm.tm_sec};
}

}  // namespace

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

    std::string save_name = options_.save_type;
    if (save_name.empty() && game_ && game_->save_type) {
        save_name = game_->save_type;
    }
    save::Type save_type = save::detect({mem_.rom.data(), mem_.rom_size});
    if (!save_name.empty()) {
        const auto parsed = save::parse(save_name);
        if (!parsed) {
            throw Error("unknown save type '" + save_name +
                        "' (expected none, sram, flash64, flash128, eeprom, eeprom512 or eeprom8k)");
        }
        save_type = *parsed;
    }
    backup_ = save::make(save_type);
    eeprom_ = backup_->is_eeprom();

    bool rtc = rtc_cartridge({mem_.rom.data(), mem_.rom_size});
    if (game_ && game_->rtc >= 0) {
        rtc = game_->rtc != 0;
    }
    if (!options_.rtc.empty()) {
        if (options_.rtc != "on" && options_.rtc != "off") {
            throw Error("unknown real-time clock setting '" + options_.rtc + "' (expected on or off)");
        }
        rtc = options_.rtc == "on";
    }
    if (rtc) {
        std::memcpy(gpio_rom_.data(), mem_.rom.data() + 0xC4, gpio_rom_.size());
        rtc_ = std::make_unique<Rtc>([this]() {
            std::tm tm{};
            if (options_.rtc_start) {
                const auto t = static_cast<std::time_t>(*options_.rtc_start + static_cast<std::int64_t>(now() / 16777216));
#ifdef _WIN32
                gmtime_s(&tm, &t);
#else
                gmtime_r(&t, &tm);
#endif
            } else {
                const std::time_t t = std::time(nullptr);
#ifdef _WIN32
                localtime_s(&tm, &t);
#else
                localtime_r(&t, &tm);
#endif
            }
            return to_clock(tm);
        });
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

void System::gpio_write(std::uint32_t addr, std::uint16_t value) {
    const std::uint32_t off = addr & (Memory::kRomMax - 2);
    if (rtc_ && off >= 0xC4 && off <= 0xC8) {
        rtc_->write(off, value);
        show_gpio();
    }
}

// The port registers are mirrored into the ROM image, so every read path (page tables,
// generated code, the interpreter) sees them without a check on each ROM access.
void System::show_gpio() {
    if (!rtc_) {
        return;
    }
    rom_sha256();  // the state-file identity is the image without the port registers
    if (rtc_->readable()) {
        const auto regs = rtc_->registers();
        std::memcpy(mem_.rom.data() + 0xC4, regs.data(), 6);
    } else {
        std::memcpy(mem_.rom.data() + 0xC4, gpio_rom_.data(), gpio_rom_.size());
    }
}

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
