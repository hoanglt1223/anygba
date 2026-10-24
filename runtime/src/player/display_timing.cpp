#include "player/display_timing.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace anygba::player {

Cadence::Cadence(double game_hz, double display_hz) : game_hz_(game_hz), display_hz_(display_hz) {
    if (!(game_hz > 0) || !(display_hz > 0) || !std::isfinite(game_hz) || !std::isfinite(display_hz)) {
        throw std::invalid_argument("frame rates must be positive");
    }
    game_uhz_ = static_cast<std::uint64_t>(std::llround(game_hz * 1e6));
    display_uhz_ = static_cast<std::uint64_t>(std::llround(display_hz * 1e6));
}

std::uint64_t Cadence::first_vblank(std::uint64_t frame) const {
    // ceil(frame * display / game) without floating-point drift; exact for runs of years.
    return (frame * display_uhz_ + game_uhz_ - 1) / game_uhz_;
}

double Cadence::position(std::uint64_t vblank) const {
    const std::uint64_t scaled = vblank * game_uhz_;
    const std::uint64_t whole = scaled / display_uhz_;
    return static_cast<double>(whole) + static_cast<double>(scaled % display_uhz_) / static_cast<double>(display_uhz_);
}

double synced_game_hz(double game_hz, double display_hz, double tolerance) {
    if (!(display_hz > 0)) return game_hz;
    for (int divisor = 1; divisor <= 4; ++divisor) {
        const double rate = display_hz / divisor;
        if (std::abs(rate / game_hz - 1.0) <= tolerance) return rate;
    }
    return game_hz;
}

int vsync_interval(double display_hz, int cap) {
    if (cap <= 0 || !(display_hz > 0)) return 1;
    return std::max(1, static_cast<int>(std::ceil(display_hz / cap - 0.01)));
}

bool same_rate(double a, double b) { return a > 0 && b > 0 && std::abs(a / b - 1.0) <= 0.01; }

double audio_ratio(double sync_factor, std::size_t queued, std::size_t target) {
    if (target == 0) return sync_factor;
    const double error = (static_cast<double>(queued) - static_cast<double>(target)) / static_cast<double>(target);
    return sync_factor * (1.0 + std::clamp(0.005 * error, -0.005, 0.005));
}

}  // namespace anygba::player
