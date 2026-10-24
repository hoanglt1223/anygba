#pragma once

#include <anyport/platform/input.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Host keys and gamepad buttons to GBA buttons and player hotkeys, from `anygba-input.ini`
// (format: docs/user/INPUT_MAPPING.md).
namespace anygba::input {

using anyport::platform::Button;

inline constexpr int kGbaButtons = 10;  // KEYINPUT bit order
inline constexpr std::array<std::string_view, kGbaButtons> kGbaButtonNames = {
    "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L"};

enum class Hotkey : std::uint8_t {
    SaveState1, SaveState2, SaveState3, SaveState4, SaveState5, SaveState6, SaveState7, SaveState8, SaveState9,
    LoadState1, LoadState2, LoadState3, LoadState4, LoadState5, LoadState6, LoadState7, LoadState8, LoadState9,
    SpeedUp,
    SpeedDown,
    Menu,
    FastForward,  // held: run unthrottled
    CheatButton,  // held: the GameShark / Action Replay device button
    AiControl,    // agent control server on/off (docs/user/AI-CONTROL.md)
    Journal,      // play journal on/off
    Stats,        // the stats overlay on/off
    Count,
};
inline constexpr int kHotkeys = static_cast<int>(Hotkey::Count);
std::string hotkey_name(Hotkey hotkey);
inline Hotkey save_slot_key(int slot) { return static_cast<Hotkey>(static_cast<int>(Hotkey::SaveState1) + slot - 1); }
inline Hotkey load_slot_key(int slot) { return static_cast<Hotkey>(static_cast<int>(Hotkey::LoadState1) + slot - 1); }

struct Mapping {
    std::array<std::vector<std::string>, kGbaButtons> keys;  // SDL key names
    std::array<std::vector<Button>, kGbaButtons> pad;
    // Keyboard chords: optional Ctrl+, Alt+, Shift+ and one key ("Shift+F1").
    std::array<std::vector<std::string>, kHotkeys> hotkeys;
    // Gamepad chords: buttons joined by '+' ("Back+Start"), all held at once.
    std::array<std::vector<std::string>, kHotkeys> pad_hotkeys;
    int speed = 100;              // [player] Speed: starting speed in percent
    int fast_forward = 300;       // [player] FastForwardSpeed: percent, 0 = unlimited
    bool stats = false;           // [player] Stats: the stats overlay is shown
    bool normal_speed_music = false;  // [player] NormalSpeedMusic: music keeps 1x tempo and pitch when faster
    int stick_deadzone = 12000;   // [player] StickDeadzone: left stick travel ignored (0-32767)
};

Mapping default_mapping();

// Starts from default_mapping(); each `Name = ...` line replaces that button's or hotkey's
// bindings for its section's device (an empty value unbinds). Throws std::runtime_error naming
// the source and line for unknown sections, names, key names, gamepad buttons or values.
Mapping parse_mapping(std::string_view text, std::string_view source_name);
Mapping load_mapping(const std::filesystem::path& path);
// Writes the mapping in the file format, e.g. to create a starting file for the user.
std::string format_mapping(const Mapping& mapping);
// Writes format_mapping() to `path` through a temporary file. Returns an error message.
std::optional<std::string> save_mapping(const Mapping& mapping, const std::filesystem::path& path);

// What the mapping reads: the real keyboard and gamepads, or a test script.
class Source {
public:
    virtual ~Source() = default;
    virtual bool key_held(std::string_view key_name) const = 0;
    virtual anyport::platform::PadState pad() const = 0;
};

// platform::Input as a Source. `input` must have no key bindings of its own
// (Input::clear_key_bindings) so its pad state reflects gamepads only.
class DeviceSource final : public Source {
public:
    explicit DeviceSource(const anyport::platform::Input& input) : input_(input) {}
    bool key_held(std::string_view key_name) const override { return input_.key_held(key_name); }
    anyport::platform::PadState pad() const override { return input_.state(); }

private:
    const anyport::platform::Input& input_;
};

// Pressed GBA buttons, KEYINPUT bit order, 1 = pressed. The left stick past the deadzone
// presses the d-pad. Gamepad buttons that belong to a held multi-button hotkey chord are left
// out, so Back+Start opening the menu does not also press Select and Start in the game.
std::uint16_t gba_keys(const Mapping& mapping, const Source& source);
// Hotkeys whose chord is held now, one bit per Hotkey. Keyboard chords need exactly their
// modifiers (F1 does not fire while Shift is held, so Shift+F1 and F1 stay apart).
std::uint32_t hotkeys_held(const Mapping& mapping, const Source& source);
inline bool has(std::uint32_t mask, Hotkey hotkey) { return (mask >> static_cast<int>(hotkey)) & 1u; }

// ---- remapping (the in-game Controls page) ----

enum class Device : std::uint8_t { Keyboard, Gamepad };

struct Target {
    bool hotkey = false;
    int index = 0;  // GBA button (0-9) or Hotkey
    bool operator==(const Target&) const = default;
};
std::string target_name(Target target);
// Bindings of `target` on `device`, comma-separated ("" when unbound).
std::string describe(const Mapping& mapping, Target target, Device device);
// A keyboard chord from the modifiers held with `key` ("Shift+F1"); a key that is itself a
// modifier is returned alone.
std::string keyboard_chord(std::string_view key, bool ctrl, bool alt, bool shift);
std::string pad_chord(const std::vector<Button>& buttons);
const char* pad_button_name(Button button);
// Binds `binding` (a keyboard chord or a gamepad chord) to `target` on `device`, replacing
// its bindings there. A GBA button takes the last button of a gamepad chord. Another target
// that had the same binding gets the target's previous binding instead (a swap) or none.
// Returns the name of that other target, if any.
std::optional<std::string> rebind(Mapping& mapping, Target target, Device device, const std::string& binding);

}  // namespace anygba::input
