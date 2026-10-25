#pragma once

#include "anygba/video_options.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anygba::video {

// The settings of a run, each source overriding the one before: defaults, the game's [video]
// config (serialized, may be null), the per-game file the menu saves, then the command line
// (serialized). A bad game config or command line throws std::runtime_error; a bad saved file
// is skipped with a message in `warning`.
Options resolve_settings(const char* game_config, const std::optional<std::filesystem::path>& saved_file,
                         const std::string& command_line, std::string* warning);

// Writes the settings for the next run. Returns an error message.
std::optional<std::string> save_settings(const std::filesystem::path& file, const Options& options);

// Graphics presets of the Video page (docs/user/VIDEO.md "Graphics presets"), cheapest first:
// each sets the filter, debanding, internal scale, layer filter, smooth rotation and motion
// smoothing. Colour, effects, HD packs, AI, widescreen, display and pacing settings stay.
inline constexpr int kPresetCount = 6;
inline constexpr int kCustomPreset = -1;
// The preset all of whose values are in effect, else kCustomPreset.
int preset_of(const Options& options);
void apply_preset(Options& options, int preset);

// The Video page of the in-game menu: one row per setting, changed with left/right.
struct MenuItem {
    std::string label;
    std::string value;
    bool toggle = false;  // an on/off switch; other values that read "off" are choices
};
std::vector<MenuItem> menu_items(const Options& options);
// Steps setting `item` (an index into menu_items) by dir (+1 or -1), wrapping around.
void menu_step(Options& options, int item, int dir);

}  // namespace anygba::video
