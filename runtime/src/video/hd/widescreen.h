#pragma once

#include "anygba/system.hpp"
#include "video/hd/frame.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Per-game widescreen rules (docs/user/WIDESCREEN.md): <game code>.toml in a widescreen/ folder.
// Each frame the first scene whose memory conditions hold decides how the extra columns are
// filled: each layer from its own map, clamped, mirrored or hidden (HUDs stay in the middle 240
// columns), or the whole picture pillarboxed. A scene may name a provider, native code that
// supplies the tile map beyond what the game keeps in VRAM.
namespace anygba::video::hd {

// "u16 0x03001234 == 0x0040": size (u8/u16/u32), address, operator (== != < <= > >= &), value.
// "&" holds when (value at address) & operand is not zero.
struct Condition {
    int size = 4;
    std::uint32_t address = 0;
    enum class Op : std::uint8_t { Eq, Ne, Lt, Le, Gt, Ge, And } op = Op::Eq;
    std::uint32_t value = 0;

    static std::optional<Condition> parse(std::string_view text, std::string* error);
    bool holds(const Memory& mem) const;
};

// Reads guest memory the way the CPU would see it (BIOS area, ROM, RAM, IO, palette, VRAM,
// OAM); 0 for unmapped addresses.
std::uint32_t peek(const Memory& mem, std::uint32_t address, int size);

// Native code that extends text BG tile maps for one game engine.
class MapProvider {
public:
    virtual ~MapProvider() = default;
    // Fills frame.extension for the layers it knows, from memory at the end of the frame.
    // Returns false when the scene does not look like what the provider understands (the caller
    // then pillarboxes rather than show wrong tiles).
    virtual bool extend(const Memory& mem, Frame& frame, int extra) = 0;
};

// Providers by name ("pokeemerald-field"); settings are the scene's [provider] keys.
std::unique_ptr<MapProvider> make_provider(std::string_view name, const std::map<std::string, std::uint32_t>& settings,
                                           std::string* error);

struct SceneRule {
    std::string name;
    std::vector<Condition> when;
    Scene scene;
    std::string provider;
    std::map<std::string, std::uint32_t> provider_settings;
};

class Rules {
public:
    // Parses a rules file. Throws std::runtime_error naming `source` and the line.
    static Rules parse(std::string_view text, const std::string& source);
    // <dir>/<code>.toml in the first of `dirs` that has one, or nullopt.
    static std::optional<Rules> find(std::string_view game_code, const std::vector<std::filesystem::path>& dirs,
                                     std::filesystem::path* found = nullptr);

    // Picks the scene for the frame that just ended and applies it to `frame` (scene and map
    // extensions). Returns the scene's name ("default" when none matched).
    const std::string& apply(const Memory& mem, Frame& frame, int extra);

    const std::string& title() const { return title_; }
    // ROM halfword patches the game needs while widescreen is on (e.g. wider sprite culling).
    const std::map<std::uint32_t, std::uint16_t>& rom_patches() const { return patches_; }
    const std::vector<SceneRule>& scenes() const { return scenes_; }

private:
    std::string title_;
    SceneRule default_;
    std::vector<SceneRule> scenes_;
    std::vector<std::unique_ptr<MapProvider>> providers_;  // by scene, null without one
    std::map<std::uint32_t, std::uint16_t> patches_;
    std::string fallback_name_;
};

}  // namespace anygba::video::hd
