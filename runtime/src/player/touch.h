#pragma once

#include <anyport/platform/window.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// On-screen touch controls (docs/user/ANDROID.md): where the picture and each control go
// for an output size, and which buttons a touch at a point presses. No SDL here; the
// overlay in touch_overlay.h feeds it events and draws it.
namespace anygba::player::touch {

using anyport::platform::Rect;

// Bits 0-9 are the GBA buttons in KEYINPUT order (A, B, Select, Start, Right, Left, Up,
// Down, R, L); the rest are player functions.
inline constexpr std::uint16_t kGbaMask = 0x03FF;
inline constexpr std::uint16_t kMenu = 1u << 10;
inline constexpr std::uint16_t kFastForward = 1u << 11;

enum class Control : std::uint8_t { DPad, A, B, L, R, Select, Start, Menu, FastForward, Count };
inline constexpr int kControls = static_cast<int>(Control::Count);

struct Settings {
    int size = 100;     // percent, 50-200
    int opacity = 45;   // percent, 10-100
    int offset_x = 0;   // percent of a button, -100-100: moves the controls in from the sides
    int offset_y = 0;   // percent of a button, -100-100: moves the bottom controls up
    bool haptics = true;
    bool operator==(const Settings&) const = default;
};

// `Name = value` lines (Size, Opacity, OffsetX, OffsetY, Haptics); '#' and ';' start
// comments. Unknown names and malformed values are skipped and values are clamped, so a
// damaged file never stops the game.
Settings parse_settings(std::string_view text);
std::string format_settings(const Settings& settings);

struct Layout {
    Rect picture;
    std::array<Rect, kControls> controls{};
    const Rect& operator[](Control c) const { return controls[static_cast<std::size_t>(c)]; }
};

// Landscape (width >= height): the picture at the largest whole scale centred in the safe
// area, controls over the sides. Portrait: the picture at the top, controls below it.
Layout layout(float width, float height, Rect safe, const Settings& settings);

// Buttons pressed by a finger at (x, y). The d-pad has eight directions and a small dead
// centre; a finger may slide a little past its edge and keep it pressed.
std::uint16_t buttons_at(const Layout& layout, float x, float y);

const char* label(Control control);

}  // namespace anygba::player::touch
