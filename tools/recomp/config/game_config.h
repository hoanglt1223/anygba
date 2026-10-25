#pragma once

#include "analysis/functions.h"
#include "analysis/image.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace anygba::config {

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct RamCode {
    analysis::RamRegion region;
    analysis::Mode mode = analysis::Mode::Arm;  // instruction set at ram_dst, which becomes an entry point
};

struct Patch {
    std::uint32_t addr = 0;
    std::vector<std::uint8_t> bytes;  // in memory order
};

// The function at `addr` is replaced by a hand-written C function `function` (same
// signature as generated ones) from one of the config's `sources`.
struct Replacement {
    std::uint32_t addr = 0;
    analysis::Mode mode = analysis::Mode::Thumb;
    std::string function;
};

// A named guest function from a [functions] table (the game config or a symbols file).
struct FunctionSymbol {
    std::uint32_t addr = 0;
    analysis::Mode mode = analysis::Mode::Thumb;
};
using FunctionTable = std::map<std::string, FunctionSymbol, std::less<>>;

// [[override]]: the function at `addr` (or the [functions] entry `symbol`) runs the C++ override
// registered as `impl` (docs/dev/OVERRIDES.md). Exactly one of addr and symbol is set.
struct Override {
    std::optional<std::uint32_t> addr;
    analysis::Mode mode = analysis::Mode::Thumb;  // with addr
    std::string symbol;
    std::string impl;
    bool enabled = true;
    std::string engine;
    bool verified = false;  // set after a passing `anygba difftest`
    std::vector<std::string> games;  // game codes (ROM header 0xAC) it applies to; empty: every game
    std::string where;  // "file:line", for error messages
};

// Per-game recompiler settings. Schema: docs/dev/GAME-CONFIG.md.
struct GameConfig {
    std::string title;
    std::vector<analysis::EntryPoint> entries;
    std::vector<RamCode> ram_code;
    std::vector<analysis::AddressRange> skip;
    std::vector<Patch> patches;
    std::string save_type;  // empty: detect at run time
    int rtc = -1;           // cartridge real-time clock: 1 on, 0 off, -1 let the runtime decide
    std::string video;      // [video] settings, serialized (anygba/video_options.h); empty: defaults
    std::vector<Replacement> replacements;
    std::vector<std::filesystem::path> sources;  // C/C++ files compiled into the game, absolute
    FunctionTable functions;
    std::vector<Override> overrides;
};

// An override module folder: overrides.toml ([[override]] entries) and the .c/.cpp files next to it.
struct OverrideModule {
    std::filesystem::path dir;
    std::vector<Override> overrides;
    std::vector<std::filesystem::path> sources;  // absolute, sorted
};

// Throws ConfigError naming the source, the key, and the line on any malformed or unknown entry.
GameConfig parse_game_config(std::string_view text, std::string_view source_name);
GameConfig load_game_config(const std::filesystem::path& path);

OverrideModule parse_override_module(std::string_view text, std::string_view source_name);
// Reads <dir>/overrides.toml and lists the module's sources. Throws ConfigError.
OverrideModule load_override_module(const std::filesystem::path& dir);

// The [functions] table of a symbols file (other tables are ignored). Throws ConfigError.
FunctionTable parse_function_symbols(std::string_view text, std::string_view source_name);
FunctionTable load_function_symbols(const std::filesystem::path& path);

// Applies patches and RAM regions to `image` and returns the discovery inputs the config adds.
analysis::DiscoveryOptions apply_game_config(const GameConfig& config, analysis::Image& image);

}  // namespace anygba::config
