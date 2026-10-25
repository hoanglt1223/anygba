#pragma once

#include "anygba/cpu.h"
#include "anygba/system.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Differential tests of function overrides (docs/dev/OVERRIDES.md): the translation of a guest
// function and its override run from identical start states, and their effects are compared.
// The gate is the ABI-level comparison (r0, r4-r11, sp, the return address, memory writes and
// contents outside the dead stack, IO writes in order, guest calls with their arguments); the
// rest of the machine state (r1-r3, r12, lr, flags, cycles, ticks, the dead stack, write order)
// is compared and reported separately.
namespace anygba::difftest {

enum class Region : std::uint8_t { Ewram, Iwram, Io, Palette, Vram, Oam, Rom };

// How one register (or the cycle budget) of a random state is chosen.
struct Value {
    enum class Kind : std::uint8_t { Any, Fixed, Range, Pointer, String, Buffer, Near };
    Kind kind = Kind::Any;
    std::uint32_t lo = 0;            // Fixed: the value; Range: lowest value
    std::uint32_t hi = 0xFFFFFFFF;   // Range: highest value
    std::uint32_t align = 1;         // power of two
    bool thumb = false;              // set bit 0 of the result
    std::vector<Region> regions;     // Pointer, String, Buffer: one is picked per state
    std::uint32_t min_len = 0;       // String: bytes before the terminator; Buffer: size
    std::uint32_t max_len = 0;
    std::uint8_t terminator = 0xFF;  // String
    std::uint8_t byte_lo = 0;        // String, Buffer: planted byte values
    std::uint8_t byte_hi = 0xFF;
    int near = -1;                   // Near: register the value is relative to
    std::int64_t near_lo = 0;        // Near: offset range
    std::int64_t near_hi = 0;
};

// Value slots of a shape: r0-r15 (r15 unused) and the cycle budget.
inline constexpr int kCyclesSlot = 16;
inline constexpr int kSlots = 17;

// Random bytes planted at a fixed address (a global the function reads), per state.
struct MemoryValue {
    std::uint32_t addr = 0;
    std::uint32_t size = 1;
    std::uint8_t byte_lo = 0;
    std::uint8_t byte_hi = 0xFF;
};

struct Shape {
    std::string name;
    std::uint32_t weight = 1;
    std::array<Value, kSlots> values;
    std::vector<MemoryValue> memory;  // planted before the registers' strings and buffers
};

// Per-function argument spec: [<function>] in a module's difftest.toml.
struct Spec {
    std::vector<Shape> shapes;
    std::uint32_t dead_stack = 256;  // bytes below the entry sp that are dead after the return
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ignore_memory;  // [begin, end), compared nowhere
    std::uint64_t tick_limit = 10000;  // scanlines of cycles before a run counts as stuck
    // Share of states in which both sides must return to the caller's lr within the tick limit;
    // below it a run without ABI differences is "insufficient", not a pass.
    double min_returned = 0.95;
};

// Outcome of a run. Exit codes of `--difftest`: pass 0, fail 1, insufficient 3 (2 is an error).
enum class Status : std::uint8_t { Pass, Fail, Insufficient };
Status classify(std::uint64_t total, std::uint64_t failed, std::uint64_t returned, double min_returned);
const char* status_name(Status status);
int exit_code(Status status);

// The [<name>] table of a spec file, also found under the function's address ("0x08008ba0").
// nullopt when the file has neither. Throws anygba::Error naming file and line for bad specs.
std::optional<Spec> parse_spec(std::string_view text, const std::string& source, std::string_view name,
                               std::uint32_t addr);

struct Poke {
    std::uint32_t addr = 0;
    std::vector<std::uint8_t> bytes;
};

// A start state: registers, N Z C V (CPSR bits 31-28), the cycle budget and memory to plant on
// top of the base machine state.
struct State {
    std::array<std::uint32_t, 16> r{};
    std::uint32_t flags = 0;
    std::int32_t cycles = 1000000;
    std::vector<Poke> pokes;
};

// Parameters a random state was drawn with, per slot: the value (Any, Range) or length (String,
// Buffer). Forcing a parameter regenerates the state with only that slot changed.
using Params = std::array<std::optional<std::uint64_t>, kSlots>;

struct Generated {
    State state;
    Params params;  // the parameters drawn
};

// Random state number `index` of shape `shape`; deterministic in (seed, index) and independent
// per slot, so forcing one slot leaves the others as they were.
Generated generate(const Spec& spec, std::size_t shape, std::uint64_t seed, std::uint64_t index, const Params& forced,
                   std::uint32_t rom_size);
// The shape random state `index` uses.
std::size_t pick_shape(const Spec& spec, std::uint64_t seed, std::uint64_t index);

// States recorded at a function's entry during play (--record-entry): the machine state at the
// first call and, per call, registers plus the memory that differs from it.
struct Recording {
    std::uint32_t entry = 0;
    std::uint32_t thumb = 0;
    std::vector<std::uint8_t> base;  // System::save_state() image
    std::vector<State> states;
};
void write_recording(const std::filesystem::path& path, const Recording& recording);
// Throws anygba::Error for a damaged or foreign file.
Recording read_recording(const std::filesystem::path& path);

// Records the entry state of a function each time it is called, until `count` states; then
// writes the file and stops the run. Uses System::on_breakpoint.
class Recorder {
public:
    Recorder(System& sys, const anygba_game& game, std::string_view function, std::uint64_t count,
             std::filesystem::path out);
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    // Writes the file if states were recorded and not written yet. Returns a message for the user.
    std::string finish();
    std::uint64_t recorded() const { return recording_.states.size(); }

private:
    void hit();

    System& sys_;
    std::uint64_t count_;
    std::filesystem::path out_;
    Recording recording_;
    std::vector<std::vector<std::uint8_t>> base_memory_;
    bool written_ = false;
};

struct Options {
    std::string function;                              // override name, or its entry address
    std::optional<std::string> impl;                   // test this registered implementation instead
    std::optional<std::filesystem::path> spec;         // argument spec for random states
    std::vector<std::filesystem::path> recorded;       // --record-entry files
    std::optional<std::filesystem::path> base_state;   // save state random states start from
    std::uint64_t states = 10000;                      // random states (with a spec)
    std::uint64_t seed = 1;
    std::uint64_t tick_limit = 0;                      // 0: the spec's value
    std::optional<std::filesystem::path> json;         // report for tools; "-" prints it
};

// Runs the differential test and prints the report. Returns exit_code() of the status: pass when
// every state passes at the ABI level and enough states returned normally. Throws anygba::Error for unusable input. Activate the game's
// overrides first (overrides::activate).
int run(System& sys, const anygba_game& game, const Options& options);

// Index of the override named `function` (its symbol, or its entry address in any notation the
// game uses), or nullopt.
std::optional<std::uint32_t> find_override(const anygba_game& game, std::string_view function);

}  // namespace anygba::difftest
