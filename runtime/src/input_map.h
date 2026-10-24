#pragma once

#include <anyport/platform/input.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Host keys and gamepad buttons to GBA buttons, from `anygba-input.ini` (format:
// docs/user/INPUT_MAPPING.md).
namespace anygba::input {

inline constexpr int kGbaButtons = 10;  // KEYINPUT bit order
inline constexpr std::array<std::string_view, kGbaButtons> kGbaButtonNames = {
    "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L"};

struct Mapping {
    std::array<std::vector<std::string>, kGbaButtons> keys;  // SDL key names
    std::array<std::vector<anyport::platform::Button>, kGbaButtons> pad;
    std::vector<std::string> fast_forward;  // keys held to run unpaced
};

Mapping default_mapping();

// Starts from default_mapping(); each `Button = ...` line replaces that button's bindings for
// its section's device (an empty value unbinds). Throws std::runtime_error naming the source
// and line for unknown sections, buttons, key names, or gamepad buttons.
Mapping parse_mapping(std::string_view text, std::string_view source_name);
Mapping load_mapping(const std::filesystem::path& path);

// Writes the mapping in the file format, e.g. to create a starting file for the user.
std::string format_mapping(const Mapping& mapping);

// Pressed GBA buttons, KEYINPUT bit order, 1 = pressed. `input` must have no key bindings of
// its own (Input::clear_key_bindings) so its pad state reflects gamepads only.
std::uint16_t gba_keys(const Mapping& mapping, const anyport::platform::Input& input);
bool fast_forward_held(const Mapping& mapping, const anyport::platform::Input& input);

}  // namespace anygba::input
