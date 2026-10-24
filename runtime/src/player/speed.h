#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// Emulation speed for the player: fixed steps like VisualBoyAdvance's frame-skip throttle,
// plus the resampler that keeps sound continuous at any of them.
namespace anygba::player {

inline constexpr std::array<int, 4> kSpeeds = {100, 200, 300, 400};

bool valid_speed(int percent);
// The next step up or down, clamped to the list ends.
int next_speed(int percent, int direction);

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
