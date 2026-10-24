#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Video settings of a generated game (docs/user/VIDEO.md). Shared by the recompiler, which
// validates the [video] table of a game TOML, and the runtime, which applies them.
namespace anygba::video {

enum class Scaler : std::uint8_t {
    Nearest,
    Bilinear,
    SharpBilinear,
    Scale2x,
    Scale3x,
    Scale4x,
    Eagle,
    Sai2x,
    SuperEagle,
    Super2xSai,
    Hq2x,
    Hq3x,
    Hq4x,
    Xbr2x,
    Xbr3x,
    Xbr4x,
    Xbrz2x,
    Xbrz3x,
    Xbrz4x,
    Xbrz5x,
    Xbrz6x,
};

enum class Effect : std::uint8_t { None, LcdGrid, Scanlines, Crt };
enum class Color : std::uint8_t { Raw, GbaLcd };
// Render resolution: Window follows the window's own size; the rest are fixed 16:9 targets.
enum class Output : std::uint8_t { Window, P720, P1080, P1440, P2160, Desktop };

struct Options {
    Scaler scaler = Scaler::Nearest;
    Effect effect = Effect::None;
    bool ghosting = false;  // blend each frame with the previous one, like the GBA's slow LCD
    Color color = Color::Raw;
    double gamma = 1.0;     // output = input^(1/gamma); 1.0 leaves colours alone
    bool deband = false;
    Output output = Output::Window;
    bool fullscreen = false;
    bool integer_scale = false;

    bool operator==(const Options&) const = default;
};

inline constexpr double kMinGamma = 0.5;
inline constexpr double kMaxGamma = 2.5;

std::span<const Scaler> all_scalers();
std::span<const Effect> all_effects();
std::span<const Color> all_colors();
std::span<const Output> all_outputs();

std::string_view name(Scaler s);
std::string_view name(Effect e);
std::string_view name(Color c);
std::string_view name(Output o);

// Integer factor a pixel-art scaler produces (1 for nearest, bilinear and sharp-bilinear).
int factor(Scaler s);
// Fixed render size of an output, or nullopt for Window and Desktop.
std::optional<std::pair<int, int>> output_size(Output o);

// "hq3x+lcd-grid+ghosting": at most one scaler, at most one screen effect, and "ghosting" in any
// order. Sets scaler, effect and ghosting (unnamed parts reset to none). Returns an error message.
std::optional<std::string> parse_filter(std::string_view spec, Options& options);
std::string filter_spec(const Options& options);

// One setting by key, as on the command line and in the game TOML: filter, color, gamma, deband,
// output, fullscreen, integer-scale. Booleans take true/false/on/off/1/0. Returns an error message.
std::optional<std::string> apply_setting(Options& options, std::string_view key, std::string_view value);

// "key=value;key=value" with every setting that differs from the defaults (embedded in generated
// games), and its inverse.
std::string serialize(const Options& options);
std::optional<std::string> apply_serialized(Options& options, std::string_view text);

}  // namespace anygba::video
