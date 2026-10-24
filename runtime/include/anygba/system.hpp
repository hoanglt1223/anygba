#pragma once

#include "anygba/cpu.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace anygba {

class Ppu;
class Apu;
namespace save {
class Backup;
}

// Raised for conditions the runtime refuses to guess about: unknown SWIs, undefined
// instructions, bad ROMs. The message names the address or ID.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Memory {
    static constexpr std::uint32_t kBiosSize = 0x4000;
    static constexpr std::uint32_t kEwramSize = 0x40000;
    static constexpr std::uint32_t kIwramSize = 0x8000;
    static constexpr std::uint32_t kIoSize = 0x400;
    static constexpr std::uint32_t kPaletteSize = 0x400;
    static constexpr std::uint32_t kVramSize = 0x18000;
    static constexpr std::uint32_t kOamSize = 0x400;
    static constexpr std::uint32_t kRomMax = 0x2000000;

    std::vector<std::uint8_t> bios = std::vector<std::uint8_t>(kBiosSize);
    std::vector<std::uint8_t> ewram = std::vector<std::uint8_t>(kEwramSize);
    std::vector<std::uint8_t> iwram = std::vector<std::uint8_t>(kIwramSize);
    std::vector<std::uint8_t> io = std::vector<std::uint8_t>(kIoSize);
    std::vector<std::uint8_t> palette = std::vector<std::uint8_t>(kPaletteSize);
    std::vector<std::uint8_t> vram = std::vector<std::uint8_t>(kVramSize);
    std::vector<std::uint8_t> oam = std::vector<std::uint8_t>(kOamSize);
    std::vector<std::uint8_t> rom;  // always kRomMax bytes; past the image it holds open-bus values
    std::uint32_t rom_size = 0;
};

// Scheduled hardware events on the cycle timeline.
enum class Event : std::uint8_t { HBlank, LineEnd, Timer0, Timer1, Timer2, Timer3, Count };

struct DmaChannel {
    std::uint32_t src = 0;
    std::uint32_t dst = 0;
    std::uint32_t count = 0;
    std::uint16_t control = 0;
    bool active = false;  // enabled and waiting for its trigger
};

struct Timer {
    std::uint16_t reload = 0;
    std::uint16_t control = 0;
    std::uint16_t counter = 0;      // value at `started`
    std::uint64_t started = 0;      // cycle the counter was last latched
};

inline constexpr int kScreenWidth = 240;
inline constexpr int kScreenHeight = 160;
// 16.78 MHz / 280896 cycles per frame.
inline constexpr double kFramesPerSecond = 16777216.0 / 280896.0;

struct Options {
    std::filesystem::path miss_log = "anygba-miss.log";
    bool log_misses = true;
    // Backup memory type ("sram", "flash64", ...; see save/save.h). Empty: the generated
    // game's config value, else detection from the ROM's library ID strings.
    std::string save_type;
};

class System {
public:
    // `game` may be null: everything then runs in the interpreter.
    System(std::vector<std::uint8_t> rom, const anygba_game* game, Options options = {});
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    anygba_cpu& cpu() { return cpu_; }
    Memory& memory() { return mem_; }
    const Options& options() const { return options_; }
    save::Backup& backup() { return *backup_; }

    // The state the BIOS leaves before jumping to the cartridge: SYS mode, stacks set,
    // PC at 0x08000000 in ARM state.
    void reset();

    // Runs from r[15] until stop() is requested from a hook. Throws anygba::Error on
    // guest faults the runtime will not paper over.
    void run();
    void stop() { stop_requested_ = true; }

    std::uint64_t now() const;  // emulated cycles since reset
    std::uint64_t frames() const { return frames_; }
    // Current picture, RGB555 with red in bits 0-4; filled by the PPU.
    std::array<std::uint16_t, kScreenWidth * kScreenHeight>& framebuffer() { return framebuffer_; }
    std::uint32_t scanline() const { return line_; }
    // Appends the sound produced so far (32768 Hz, interleaved L/R signed 16-bit) to `out`.
    // Undrained sound is capped at about one second.
    void drain_audio(std::vector<std::int16_t>& out);
    static constexpr int kAudioRate = 32768;
    // DirectSound FIFO pops that found the FIFO empty after it had started playing (an
    // audible gap: the game's sound DMA did not keep up).
    std::uint64_t fifo_underruns() const;

    std::function<void(System&)> on_vblank;       // start of every VBlank
    std::function<void(System&)> on_idle_loop;    // a branch-to-self was reached

    // Addresses the interpreter had to run because no recompiled function started there.
    const std::set<std::uint32_t>& misses() const { return misses_; }

    // Buttons held, in KEYINPUT bit order (bit 0 A ... bit 9 L), 1 = pressed.
    void set_keys(std::uint16_t pressed_mask);

    std::uint16_t io_read16(std::uint32_t offset) const;
    void io_write16(std::uint32_t offset, std::uint16_t value);

    // ---- used by the C ABI glue (anygba_* functions) ----
    std::uint32_t read32(std::uint32_t addr);
    std::uint32_t read16(std::uint32_t addr);
    std::uint32_t read8(std::uint32_t addr);
    void write32(std::uint32_t addr, std::uint32_t value);
    void write16(std::uint32_t addr, std::uint32_t value);
    void write8(std::uint32_t addr, std::uint32_t value);

    void tick();
    void idle_loop();
    void swi(std::uint32_t number);
    void set_cpsr(std::uint32_t value);
    void msr(std::uint32_t value, std::uint32_t mask, bool to_spsr);
    void restore_cpsr();
    int resume(std::uint32_t expected);
    [[noreturn]] void unwind();
    // Stops run() with anygba::Error(message); usable from inside generated code.
    [[noreturn]] void fail(std::string message);
    void run_once();
    int run_guarded();
    anygba_fn lookup(std::uint32_t addr, bool thumb) const;

    // Interpreter: executes from r[15] until it reaches a recompiled function or an
    // address an active call site expects. Used for misses and as the test oracle.
    void interpret();

private:
    friend struct SystemAccess;

    void map_pages();
    void update_wait_states();
    void schedule(Event e, std::uint64_t at);
    void reschedule_budget();
    void request_tick();  // make the next AG_TICK check fire without moving time
    void run_events();
    void on_hblank();
    void on_line_end();
    void raise_irq(std::uint16_t bits);
    void deliver_irq();
    void halt_until_irq(std::uint16_t wake_mask);
    void check_keypad_irq();
    void stub_swi(std::uint32_t number, std::uint32_t call_pc);
    void intr_wait(bool discard_old, std::uint16_t flags);
    void dma_write_control(int channel, std::uint16_t value);
    void dma_trigger(int timing);
public:
    // Sound FIFO A (0) or B (1) wants data: runs the DMA channel (1 or 2) in FIFO mode
    // that targets it. Called by the APU on timer overflow.
    void request_fifo_dma(int fifo);
private:
    void run_dma(int channel);
    std::uint16_t timer_counter(int timer) const;
    void timer_write_control(int timer, std::uint16_t value);
    void on_timer_overflow(int timer);
    void schedule_timer(int timer);
    void log_miss(std::uint32_t addr, bool thumb);
    std::uint32_t open_bus() const;
    bool eeprom_at(std::uint32_t addr) const;
    std::uint16_t eeprom_read();
    void eeprom_write(std::uint32_t value);

    anygba_cpu cpu_{};
    Memory mem_;
    Options options_;
    const anygba_game* game_ = nullptr;
    std::vector<const anygba_function_entry*> rom_direct_;  // by (addr - 0x08000000) / 2
    std::vector<anygba_function_entry> ram_functions_;

    // Timeline: cpu_.cycles counts down from budget_ towards the next event.
    std::uint64_t budget_start_ = 0;
    std::int32_t budget_ = 0;
    std::array<std::uint64_t, static_cast<int>(Event::Count)> events_{};
    std::uint32_t line_ = 0;
    bool hblank_ = false;
    std::uint64_t frames_ = 0;
    bool halted_ = false;
    bool in_irq_ = false;

    std::array<DmaChannel, 4> dma_{};
    std::array<std::uint16_t, kScreenWidth * kScreenHeight> framebuffer_{};
    std::unique_ptr<Ppu> ppu_;
    std::unique_ptr<Apu> apu_;
    std::array<Timer, 4> timers_{};
    std::unique_ptr<save::Backup> backup_;
    bool eeprom_ = false;
    std::uint32_t eeprom_dma_count_ = 0;  // length of the DMA 3 transfer in progress, else 0

    bool stop_requested_ = false;
    std::string error_;
    std::set<std::uint32_t> misses_;
    std::FILE* miss_file_ = nullptr;
    void* top_jmp_ = nullptr;  // jmp_buf of run(), while running
    std::uint32_t last_bios_opcode_ = 0xE129F000;
    std::uint64_t stubs_logged_ = 0;  // stubbed SWI numbers already reported
};

System& system_of(anygba_cpu* c);

}  // namespace anygba
