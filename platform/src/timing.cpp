#include <anyport/platform/timing.h>

#include <anyport/platform/context.h>

#include <SDL3/SDL_timer.h>

#include <cmath>

namespace anyport::platform {

std::uint64_t now_ns() {
    return SDL_GetTicksNS();
}

void sleep_ns(std::uint64_t duration) {
    SDL_DelayPrecise(duration);
}

FramePacer::FramePacer(double frames_per_second) {
    if (!(frames_per_second > 0.0) || !std::isfinite(frames_per_second)) {
        throw Error("frame rate must be a positive finite number");
    }
    period_ns_ = static_cast<std::uint64_t>(std::llround(1e9 / frames_per_second));
    if (period_ns_ == 0) {
        throw Error("frame rate is too high");
    }
}

void FramePacer::wait() {
    const std::uint64_t now = now_ns();
    if (next_ns_ == 0 || now > next_ns_ + period_ns_) {
        next_ns_ = now + period_ns_;
        return;
    }
    if (now < next_ns_) {
        sleep_ns(next_ns_ - now);
    }
    next_ns_ += period_ns_;
}

}  // namespace anyport::platform
