#pragma once

#include "anygba/cpu.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace anygba {

class Ppu;
class Apu;
class Rtc;
namespace save {
class Backup;
}
namespace state {
class Archive;
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

// Where the HLE BIOS IRQ dispatcher sits after `bx` to the game's handler.
inline constexpr std::uint32_t kBiosIrqReturn = 0x138;

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
    // Cartridge real-time clock: "on", "off", or empty for the generated game's config value,
    // else the built-in list of games that shipped with one.
    std::string rtc;
    // Unix time the clock starts at; it then advances with emulated time, so runs repeat.
    // Unset: the host's local time.
    std::optional<std::int64_t> rtc_start;
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
    bool has_rtc() const { return rtc_ != nullptr; }

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
    // Emulated cycles spent in interpret() (code the recompiler missed), for the stats overlay.
    std::uint64_t interpreted_cycles() const { return interpreted_cycles_; }

    std::function<void(System&)> on_vblank;       // start of every VBlank
    std::function<void(System&)> on_idle_loop;    // a branch-to-self was reached
    // A visible line (0-159) was just rendered, with the IO registers, palette, OAM and VRAM it
    // used still in memory(); the HD renderer snapshots them here.
    std::function<void(System&, unsigned line)> on_line_rendered;
    // Internal BG2X, BG2Y, BG3X, BG3Y reference points (20.8 fixed) the last rendered line used.
    std::array<std::int32_t, 4> affine_reference() const;
    // Changes whenever VRAM may have changed (a write, a reset or a state load).
    std::uint64_t vram_writes() const { return vram_writes_; }
    void mark_vram_written() { ++vram_writes_; }

    // Addresses the interpreter had to run because no recompiled function started there.
    const std::set<std::uint32_t>& misses() const { return misses_; }

    // Buttons held, in KEYINPUT bit order (bit 0 A ... bit 9 L), 1 = pressed.
    void set_keys(std::uint16_t pressed_mask);

    // ---- save states (docs/dev/SAVE-STATES.md) ----
    // A complete snapshot: CPU, memory, scheduler, video, sound, DMA, timers, backup chip.
    // Tied to the ROM by its SHA-256.
    std::vector<std::uint8_t> save_state();
    // Replaces the machine state with `image`. Throws anygba::Error for a damaged file, another
    // ROM, or another format version, and then leaves the state unchanged. While run() is
    // active, call it only from a safe-point action.
    void load_state(std::span<const std::uint8_t> image);
    // Runs `action` at the next safe point, then restarts dispatch at r[15]. Safe points are
    // the ends of scheduler ticks outside interrupt handlers, where every guest register is in
    // cpu() and r[15] names the next instruction, so a snapshot taken there can be resumed
    // without the host call frames of generated code. Exceptions from `action` stop run().
    void request_safe_point(std::function<void(System&)> action);
    bool safe_point_pending() const { return static_cast<bool>(safe_point_); }
    static constexpr std::uint32_t kStateVersion = 2;
    // SHA-256 of the unpatched ROM image.
    const std::string& rom_sha256();

    // ---- ROM patch cheats (docs/dev/CHEATS.md) ----
    // Replaces the active ROM patches: cartridge halfword address -> value. Generated
    // functions whose code covers a patched address run in the interpreter from their next
    // entry on; a body already running finishes as compiled.
    void set_rom_patches(const std::map<std::uint32_t, std::uint16_t>& patches);
    const std::map<std::uint32_t, std::uint16_t>& rom_patches() const { return rom_patches_; }
    bool rom_bypassed(std::uint32_t entry) const { return bypassed_.contains(entry); }

    std::uint16_t io_read16(std::uint32_t offset) const;
    void io_write16(std::uint32_t offset, std::uint16_t value);

    // ---- debugger hooks (agent control: docs/dev/control-protocol.md) ----
    struct Access {
        std::uint32_t addr = 0;
        std::uint32_t value = 0;  // read or written
        std::uint8_t size = 0;    // 1, 2 or 4
        bool write = false;
    };
    // Called after every bus access in a watched region (bit n of the mask: 0x0n000000 -
    // 0x0nFFFFFF). Watched regions leave the inline fast path, so generated code's accesses
    // reach the bus and the hook as well.
    std::function<void(System&, const Access&)> on_watch;
    void set_watched_regions(std::uint16_t mask);
    // Called before the interpreter runs the instruction at a breakpoint, and when a generated
    // ROM function whose entry is a breakpoint starts (its body runs compiled, so addresses
    // inside it cannot stop).
    std::function<void(System&, std::uint32_t)> on_breakpoint;
    void set_breakpoints(std::set<std::uint32_t> addrs);
    const std::set<std::uint32_t>& breakpoints() const { return breakpoints_; }
    void hit_breakpoint(std::uint32_t addr) {
        if (on_breakpoint && breakpoints_.contains(addr)) on_breakpoint(*this, addr);
    }
    const anygba_game* game() const { return game_; }

    // ---- differential tests of overrides (runtime/src/overrides/difftest.cpp) ----
    // The machine state without save_state()'s header and checks, for restoring it many times.
    std::vector<std::uint8_t> snapshot();
    void restore(std::span<const std::uint8_t> snapshot);
    // While on, tick() runs no events and delivers no interrupts: it counts ticks and adds one
    // scanline (1232 cycles) of budget, and fails with a timeout past `limit` ticks. Halting for
    // an interrupt fails. Code then runs the same however long it takes.
    void isolate_ticks(bool on, std::uint64_t limit = 0);
    std::uint64_t isolated_ticks() const { return isolated_ticks_; }
    // Runs `fn` (generated code or an override entry) from cpu() with run()'s error handling,
    // outside run(). Returns "" when it returned, else why it stopped.
    std::string call_isolated(anygba_fn fn);

    // ---- used by the C ABI glue (anygba_* functions) ----
    std::uint32_t read32(std::uint32_t addr) { return watched(addr, bus_read32(addr), 4, false); }
    std::uint32_t read16(std::uint32_t addr) { return watched(addr, bus_read16(addr), 2, false); }
    std::uint32_t read8(std::uint32_t addr) { return watched(addr, bus_read8(addr), 1, false); }
    void write32(std::uint32_t addr, std::uint32_t value) {
        bus_write32(addr, value);
        watched(addr, value, 4, true);
    }
    void write16(std::uint32_t addr, std::uint32_t value) {
        bus_write16(addr, value);
        watched(addr, value, 2, true);
    }
    void write8(std::uint32_t addr, std::uint32_t value) {
        bus_write8(addr, value);
        watched(addr, value, 1, true);
    }

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

    std::uint32_t bus_read32(std::uint32_t addr);
    std::uint32_t bus_read16(std::uint32_t addr);
    std::uint32_t bus_read8(std::uint32_t addr);
    void bus_write32(std::uint32_t addr, std::uint32_t value);
    void bus_write16(std::uint32_t addr, std::uint32_t value);
    void bus_write8(std::uint32_t addr, std::uint32_t value);
    std::uint32_t watched(std::uint32_t addr, std::uint32_t value, std::uint8_t size, bool write) {
        if ((addr >> 28) == 0 && ((watched_regions_ >> (addr >> 24)) & 1u) && on_watch) [[unlikely]] {
            on_watch(*this, Access{addr, value, size, write});
        }
        return value;
    }

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
    void serialize(state::Archive& ar);
    void run_safe_point();
    std::uint32_t open_bus() const;
    bool in_irq_handler() const;
    bool eeprom_at(std::uint32_t addr) const;
    std::uint16_t eeprom_read();
    void eeprom_write(std::uint32_t value);
    void gpio_write(std::uint32_t addr, std::uint16_t value);
    void show_gpio();

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
    std::uint64_t vram_writes_ = 0;
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
    std::unique_ptr<Rtc> rtc_;
    std::array<std::uint8_t, 6> gpio_rom_{};  // ROM bytes at 0xC4-0xC9 under the GPIO registers

    bool stop_requested_ = false;
    std::string error_;
    std::set<std::uint32_t> misses_;
    std::FILE* miss_file_ = nullptr;
    void* top_jmp_ = nullptr;  // jmp_buf of run(), while running
    static constexpr std::uint32_t kMaxResumeDepth = 512;
    std::uint32_t resume_depth_ = 0;  // nested resume() calls on the host stack
    std::uint32_t last_bios_opcode_ = 0xE129F000;
    std::uint64_t stubs_logged_ = 0;  // stubbed SWI numbers already reported
    std::function<void(System&)> safe_point_;
    // Dispatch after a safe point restarts mid-function; that address is not a miss.
    bool resuming_ = false;
    std::uint64_t interpreted_cycles_ = 0;
    static constexpr std::uint32_t kNoSwi = 0xFFFFFFFF;
    std::uint32_t swi_pc_ = kNoSwi;  // the SWI being executed, for safe points in its halt
    std::string rom_sha256_;  // computed on first use
    std::map<std::uint32_t, std::uint16_t> rom_patches_;
    std::map<std::uint32_t, std::uint16_t> rom_originals_;
    std::set<std::uint32_t> bypassed_;  // generated function entries covered by a ROM patch
    std::uint16_t watched_regions_ = 0;
    bool isolated_ = false;
    std::uint64_t isolated_ticks_ = 0;
    std::uint64_t isolated_limit_ = 0;
    int call_guarded(anygba_fn fn);
    std::set<std::uint32_t> breakpoints_;
    anygba_fn lookup_compiled(std::uint32_t addr, bool thumb) const;  // ignores ROM patches and RAM contents
    const anygba_function_entry* ram_function(std::uint32_t addr, bool thumb) const;
public:
    // A generated RAM function at `entry` exists and RAM holds the bytes it was compiled from.
    bool ram_code_valid(std::uint32_t entry, bool thumb) const;
};

System& system_of(anygba_cpu* c);

}  // namespace anygba
