#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Emulation speed for the player: fixed steps like VisualBoyAdvance's frame-skip throttle,
// plus the resampler that keeps sound continuous at any of them.
namespace anygba::player {

inline constexpr std::array<int, 4> kSpeeds = {100, 200, 300, 400};

bool valid_speed(int percent);
// The next step up or down, clamped to the list ends.
int next_speed(int percent, int direction);

// Fast-forward rates while the hotkey is held; kUnlimited runs as fast as the host can and
// drops the sound the device cannot keep up with.
inline constexpr int kUnlimited = 0;
inline constexpr std::array<int, 7> kFastForwardSpeeds = {150, 200, 300, 400, 500, 800, kUnlimited};
inline constexpr int kDefaultFastForward = 300;
// "300" or "unlimited"; nullopt for anything else.
std::optional<int> parse_fast_forward(std::string_view text);
// "300%" or "unlimited"; without the percent sign for the settings file.
std::string format_fast_forward(int percent, bool with_percent = true);
// Steps through kFastForwardSpeeds, clamped to its ends (unlimited is the last).
int next_fast_forward(int percent, int direction);

// Stereo 16-bit linear-interpolation resampler for fast play: every input frame advances
// the read position by 1, every output frame by `step` (2.0 at 200%), so N frames in give
// about N / step frames out and the pitch rises with the speed. The fractional position and
// the last input frame carry over between calls, so block edges do not click.
class Resampler {
public:
    void process(std::span<const std::int16_t> in, double step, std::vector<std::int16_t>& out);
    void reset();

private:
    double pos_ = 0;  // read position in the current block; -1..0 interpolates from last_
    std::int16_t last_[2] = {0, 0};
};

}  // namespace anygba::player
