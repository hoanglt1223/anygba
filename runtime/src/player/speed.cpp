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

void TimeStretch::process(std::span<const std::int16_t> in, double ratio, std::vector<std::int16_t>& out) {
    constexpr int hop = kSegment / 2;
    static const std::array<float, kSegment> window = [] {
        std::array<float, kSegment> w{};
        for (int i = 0; i < kSegment; ++i) {
            w[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / kSegment));
        }
        return w;
    }();
    in_.insert(in_.end(), in.begin(), in.end());
    if (tail_.empty()) tail_.assign(static_cast<std::size_t>(hop) * 2, 0.0f);
    const auto frames = [this] { return static_cast<std::ptrdiff_t>(in_.size() / 2); };
    const auto mono = [this](std::ptrdiff_t f) {
        return in_[static_cast<std::size_t>(f) * 2] + in_[static_cast<std::size_t>(f) * 2 + 1];
    };
    for (;;) {
        const auto nominal = static_cast<std::ptrdiff_t>(std::llround(next_));
        const std::ptrdiff_t lo = natural_ < 0 ? nominal : std::max<std::ptrdiff_t>(0, nominal - kSearch);
        const std::ptrdiff_t hi = natural_ < 0 ? nominal : nominal + kSearch;
        if (hi + kSegment > frames()) break;
        std::ptrdiff_t start = nominal;
        if (natural_ >= 0) {
            // Where the last segment would have gone on: the best match continues its waveform.
            const std::ptrdiff_t natural = natural_;
            double best = -1e300;
            for (std::ptrdiff_t s = lo; s <= hi; s += 2) {
                double corr = 0, energy = 1e-9;
                for (std::ptrdiff_t k = 0; k < hop; k += 4) {
                    const double a = mono(s + k);
                    corr += a * mono(natural + k);
                    energy += a * a;
                }
                const double score = corr / std::sqrt(energy);
                if (score > best) {
                    best = score;
                    start = s;
                }
            }
        }
        for (std::ptrdiff_t i = 0; i < hop; ++i) {
            const auto w = static_cast<std::size_t>(i);
            for (std::size_t ch = 0; ch < 2; ++ch) {
                const float v = tail_[w * 2 + ch] + in_[static_cast<std::size_t>(start + i) * 2 + ch] * window[w];
                out.push_back(static_cast<std::int16_t>(std::clamp(std::lround(v), -32768L, 32767L)));
                tail_[w * 2 + ch] = in_[static_cast<std::size_t>(start + hop + i) * 2 + ch] * window[w + hop];
            }
        }
        natural_ = start + hop;
        next_ += hop * ratio;
    }
    // Drop input no later segment can use.
    std::ptrdiff_t keep = static_cast<std::ptrdiff_t>(std::floor(next_)) - kSearch;
    if (natural_ >= 0) keep = std::min(keep, natural_);
    keep = std::clamp<std::ptrdiff_t>(keep, 0, frames());
    if (keep > 0) {
        in_.erase(in_.begin(), in_.begin() + keep * 2);
        next_ -= static_cast<double>(keep);
        if (natural_ >= 0) natural_ -= keep;
    }
}

void TimeStretch::reset() {
    in_.clear();
    tail_.clear();
    next_ = 0;
    natural_ = -1;
}

}  // namespace anygba::player
