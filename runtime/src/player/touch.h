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

using anyport::platform::FRect;

// Bits 0-9 are the GBA buttons in KEYINPUT order (A, B, Select, Start, Right, Left, Up,
// Down, R, L); the rest are player functions.
inline constexpr std::uint16_t kGbaMask = 0x03FF;
inline constexpr std::uint16_t kMenu = 1u << 10;
inline constexpr std::uint16_t kFastForward = 1u << 11;

enum class Control : std::uint8_t { DPad, A, B, L, R, Select, Start, Menu, FastForward, Count };
inline constexpr int kControls = static_cast<int>(Control::Count);

// One control moved and resized in the layout editor, relative to its default place.
struct Adjust {
    int x = 0, y = 0;  // hundredths of a button unit, -1000-1000
    int size = 100;    // percent, 50-200
    bool operator==(const Adjust&) const = default;
};

// Where the floating menu button rests: an edge and a position along it.
enum class Edge : std::uint8_t { Left, Right, Top, Bottom };

struct Settings {
    int size = 100;     // percent, 50-200
    int opacity = 45;   // percent, 10-100
    int offset_x = 0;   // percent of a button, -100-100: moves the controls in from the sides
    int offset_y = 0;   // percent of a button, -100-100: moves the bottom controls up
    bool haptics = true;
    bool floating_button = true;  // the floating menu button replaces the MENU control
    bool show_controls = true;    // the game buttons are drawn and pressed
    Edge button_edge = Edge::Right;
    int button_pos = 300;         // permille along the edge, from its top or left end
    std::array<Adjust, kControls> adjust{};
    bool operator==(const Settings&) const = default;
};

// `Name = value` lines (Size, Opacity, OffsetX, OffsetY, Haptics, FloatingButton,
// ShowControls, ButtonEdge, ButtonPos, and <Control>X, <Control>Y, <Control>Size per control);
// '#' and ';' start comments. Unknown names and malformed values are skipped and values are
// clamped, so a damaged file never stops the game.
Settings parse_settings(std::string_view text);
std::string format_settings(const Settings& settings);

struct Layout {
    FRect picture;
    std::array<FRect, kControls> controls{};
    FRect safe;
    float unit = 0;            // the button unit Adjust offsets are measured in
    bool menu_button = true;   // the MENU control is in use (no floating button)
    bool show_controls = true;
    const FRect& operator[](Control c) const { return controls[static_cast<std::size_t>(c)]; }
};

// Landscape (width >= height): the picture at the largest whole scale centred in the safe
// area, controls over the sides. Portrait: the picture at the top, controls below it.
// `picture_width` is the game picture's width in GBA pixels (wider than 240 in widescreen).
// Each control is then moved and resized by its Adjust and kept inside the safe area.
Layout layout(float width, float height, FRect safe, const Settings& settings, float picture_width = 240);

// Buttons pressed by a finger at (x, y). The d-pad has eight directions and a small dead
// centre; a finger may slide a little past its edge and keep it pressed. Hidden controls
// press nothing; MENU only while it is in use.
std::uint16_t buttons_at(const Layout& layout, float x, float y);

// The control drawn at (x, y), for the layout editor (hidden ones included); Count for none.
Control control_at(const Layout& layout, float x, float y);

const char* label(Control control);
// Settings-file name of a control: "DPad", "A", ..., "FastForward".
const char* key_name(Control control);

}  // namespace anygba::player::touch
