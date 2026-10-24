#pragma once

#include "analysis/functions.h"
#include "analysis/image.h"

#include <cstdint>
#include <filesystem>
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

// Per-game recompiler settings. Schema: docs/dev/GAME-CONFIG.md.
struct GameConfig {
    std::string title;
    std::vector<analysis::EntryPoint> entries;
    std::vector<RamCode> ram_code;
    std::vector<analysis::AddressRange> skip;
    std::vector<Patch> patches;
    std::string save_type;  // empty: detect at run time
    std::vector<Replacement> replacements;
    std::vector<std::filesystem::path> sources;  // C files compiled into the game, absolute
};

// Throws ConfigError naming the source, the key, and the line on any malformed or unknown entry.
GameConfig parse_game_config(std::string_view text, std::string_view source_name);
GameConfig load_game_config(const std::filesystem::path& path);

// Applies patches and RAM regions to `image` and returns the discovery inputs the config adds.
analysis::DiscoveryOptions apply_game_config(const GameConfig& config, analysis::Image& image);

}  // namespace anygba::config
