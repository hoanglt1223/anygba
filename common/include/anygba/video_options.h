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
// Real-time AI upscaling (docs/user/HD-PACKS.md): none, the final frame, or each layer.
enum class Ai : std::uint8_t { Off, Frame, Layers };
// Upscaling models, downloaded on first use (docs/user/HD-PACKS.md).
enum class AiModel : std::uint8_t { GeneralX4v3, X4Plus };
// Picture shape (docs/user/WIDESCREEN.md): the GBA's 3:2, or wider through extra columns.
enum class Widescreen : std::uint8_t { Off, W16x10, W16x9 };

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
    // HD rendering (docs/user/HD-PACKS.md): the PPU composes the layers at internal_scale x.
    int internal_scale = 1;
    Scaler layer_filter = Scaler::Nearest;  // scaler applied to each layer before compositing
    bool smooth_affine = true;  // rotated and scaled layers/sprites sampled at the internal resolution
    bool hd_pack = true;        // replace art from hdpacks/<game code>/ when that folder exists
    bool hd_dump = false;       // write every new tile, sprite and bitmap to the dump folder
    Ai ai = Ai::Off;
    AiModel ai_model = AiModel::GeneralX4v3;
    Widescreen widescreen = Widescreen::Off;
    // Pictures between frames on high-refresh displays, from the PPU's scroll and sprite motion.
    bool smoothing = false;
    // Refresh and frame pacing (docs/user/VIDEO.md#refresh-and-frame-pacing).
    int refresh = 0;               // monitor refresh in Hz the pacing assumes; 0 = detect
    bool vrr = false;              // variable refresh monitor: present each frame when it is ready
    bool sync_to_monitor = false;  // run the game at the monitor's rate when within 1%
    int menu_fps = 30;             // pictures per second while the menu is open
    int fps_cap = 0;               // most pictures per second shown; 0 = no cap

    bool operator==(const Options&) const = default;
};

inline constexpr double kMinGamma = 0.5;
inline constexpr double kMaxGamma = 2.5;
inline constexpr int kMaxInternalScale = 8;
inline constexpr int kRefreshRates[] = {0, 60, 75, 90, 120, 144, 165, 240};
inline constexpr int kMenuFps[] = {30, 60};
inline constexpr int kFpsCaps[] = {0, 30, 60, 120};

std::span<const Scaler> all_scalers();
std::span<const Effect> all_effects();
std::span<const Color> all_colors();
std::span<const Output> all_outputs();
std::span<const Ai> all_ai_modes();
std::span<const AiModel> all_ai_models();
std::span<const Widescreen> all_widescreens();

std::string_view name(Scaler s);
std::string_view name(Effect e);
std::string_view name(Color c);
std::string_view name(Output o);
std::string_view name(Ai a);
std::string_view name(AiModel m);
std::string_view name(Widescreen w);

// Columns added on each side of the 240-pixel picture by a widescreen mode (0 when off): the
// picture becomes (240 + 2 * extra) x 160.
int widescreen_extra(Widescreen w);

// Integer factor a pixel-art scaler produces (1 for nearest, bilinear and sharp-bilinear).
int factor(Scaler s);
// Fixed render size of an output, or nullopt for Window and Desktop.
std::optional<std::pair<int, int>> output_size(Output o);

// "hq3x+lcd-grid+ghosting": at most one scaler, at most one screen effect, and "ghosting" in any
// order. Sets scaler, effect and ghosting (unnamed parts reset to none). Returns an error message.
std::optional<std::string> parse_filter(std::string_view spec, Options& options);
std::string filter_spec(const Options& options);

// One setting by key, as on the command line and in the game TOML: filter, color, gamma, deband,
// output, fullscreen, integer-scale, internal-scale, layer-filter, smooth-affine, hd-pack, hd-dump,
// ai, ai-model, widescreen, smoothing, refresh (auto or Hz), vrr, sync-to-monitor, menu-fps,
// fps-cap (off or fps). Booleans take true/false/on/off/1/0. Returns an error message.
std::optional<std::string> apply_setting(Options& options, std::string_view key, std::string_view value);

// "key=value;key=value" with every setting that differs from the defaults (embedded in generated
// games), and its inverse.
std::string serialize(const Options& options);
std::optional<std::string> apply_serialized(Options& options, std::string_view text);

}  // namespace anygba::video
