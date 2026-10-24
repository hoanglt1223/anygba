#include "player/speed.h"

#include <algorithm>
#include <cmath>

namespace anygba::player {

bool valid_speed(int percent) { return std::find(kSpeeds.begin(), kSpeeds.end(), percent) != kSpeeds.end(); }

int next_speed(int percent, int direction) {
    const auto it = std::find(kSpeeds.begin(), kSpeeds.end(), percent);
    if (it == kSpeeds.end()) {
        return kSpeeds.front();
    }
    const auto index = static_cast<int>(it - kSpeeds.begin()) + direction;
    return kSpeeds[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(kSpeeds.size()) - 1))];
}

std::optional<int> parse_fast_forward(std::string_view text) {
    if (text == "unlimited") return kUnlimited;
    if (text.empty() || text.size() > 4) return std::nullopt;
    int value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return std::nullopt;
        value = value * 10 + (ch - '0');
    }
    if (value == kUnlimited ||
        std::find(kFastForwardSpeeds.begin(), kFastForwardSpeeds.end(), value) == kFastForwardSpeeds.end()) {
        return std::nullopt;
    }
    return value;
}

std::string format_fast_forward(int percent, bool with_percent) {
    if (percent == kUnlimited) return "unlimited";
    return std::to_string(percent) + (with_percent ? "%" : "");
}

int next_fast_forward(int percent, int direction) {
    const auto it = std::find(kFastForwardSpeeds.begin(), kFastForwardSpeeds.end(), percent);
    if (it == kFastForwardSpeeds.end()) return kDefaultFastForward;
    const auto index = static_cast<int>(it - kFastForwardSpeeds.begin()) + direction;
    return kFastForwardSpeeds[static_cast<std::size_t>(
        std::clamp(index, 0, static_cast<int>(kFastForwardSpeeds.size()) - 1))];
}

void Resampler::process(std::span<const std::int16_t> in, double step, std::vector<std::int16_t>& out) {
    const std::size_t frames = in.size() / 2;
    if (frames == 0) {
        return;
    }
    const auto sample = [&](std::ptrdiff_t frame, int channel) -> double {
        return frame < 0 ? last_[channel] : in[static_cast<std::size_t>(frame) * 2 + static_cast<std::size_t>(channel)];
    };
    const auto last_index = static_cast<double>(frames - 1);
    while (pos_ <= last_index) {
        const double base = std::floor(pos_);
        const double frac = pos_ - base;
        const auto i = static_cast<std::ptrdiff_t>(base);
        for (int ch = 0; ch < 2; ++ch) {
            const double a = sample(i, ch);
            const double b = frac > 0 ? sample(i + 1, ch) : a;
            out.push_back(static_cast<std::int16_t>(std::lround(a + (b - a) * frac)));
        }
        pos_ += step;
    }
    pos_ -= static_cast<double>(frames);
    last_[0] = in[(frames - 1) * 2];
    last_[1] = in[(frames - 1) * 2 + 1];
}

void Resampler::reset() {
    pos_ = 0;
    last_[0] = last_[1] = 0;
}

}  // namespace anygba::player
