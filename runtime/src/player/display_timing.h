#pragma once

#include <cstddef>
#include <cstdint>

// Frame pacing against the monitor (docs/user/VIDEO.md#refresh-and-frame-pacing). The game runs
// at its own rate (the GBA's 59.73 frames per second); the monitor refreshes at another. Each
// game frame is shown from the first refresh (vblank) at or after its time, so frames stay on
// screen for evenly spread numbers of refreshes.
namespace anygba::player {

class Cadence {
public:
    // Rates in frames per second; both must be positive.
    Cadence(double game_hz, double display_hz);

    // Display vblank, counted from 0 at frame 0, on which game frame `frame` first shows.
    std::uint64_t first_vblank(std::uint64_t frame) const;
    // Vblanks frame `frame` stays on screen (0 when the display is slower and it is skipped).
    unsigned vblanks(std::uint64_t frame) const { return static_cast<unsigned>(first_vblank(frame + 1) - first_vblank(frame)); }
    // Position of display vblank `vblank` in game frames: frame + fraction.
    double position(std::uint64_t vblank) const;

    double game_hz() const { return game_hz_; }
    double display_hz() const { return display_hz_; }

private:
    double game_hz_, display_hz_;
    std::uint64_t game_uhz_, display_uhz_;  // micro-hertz, for exact integer schedules
};

// "Sync game to monitor": the rate to run the game at so that each frame takes a whole number of
// vblanks, when that is within `tolerance` (1%) of the game's own rate: the monitor's rate divided
// by 1 to 4. Otherwise the game's own rate.
double synced_game_hz(double game_hz, double display_hz, double tolerance = 0.01);
inline bool can_sync(double game_hz, double display_hz, double tolerance = 0.01) {
    return synced_game_hz(game_hz, display_hz, tolerance) != game_hz;
}

// Present every `interval`-th vblank so at most `cap` pictures a second are shown (0: no cap).
int vsync_interval(double display_hz, int cap);

// Two rates are the same monitor rate (within 1%).
bool same_rate(double a, double b);

// Speed factor for the sound (1 = as emulated) that keeps the output queue near `target`
// frames while the game is paced by the display: the game's speed-up from syncing, nudged by at
// most 0.5% toward the target (dynamic rate control).
double audio_ratio(double sync_factor, std::size_t queued, std::size_t target);

}  // namespace anygba::player
