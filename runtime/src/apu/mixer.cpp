#include "apu/apu.h"

#include <algorithm>
#include <cmath>

// Mixing (GBATEK "GBA Sound Control Registers"): PSG channels are summed per side, scaled
// by the 0-7 master volume and the 25/50/100% PSG volume; each FIFO adds its sample at
// 50% (x2) or 100% (x4), so a FIFO spans the full +/-0x200; SOUNDBIAS is added and the
// result clipped to the 10-bit DAC range 0..0x3FF, then centred on 0x200 for 16-bit output.
namespace anygba {

void Apu::produce_sample() {
    const double psg[4] = {
        run_square(square_[0], kCyclesPerSample) / double{kCyclesPerSample},
        run_square(square_[1], kCyclesPerSample) / double{kCyclesPerSample},
        run_wave(kCyclesPerSample) / double{kCyclesPerSample},
        run_noise(kCyclesPerSample) / double{kCyclesPerSample},
    };
    double left = 0;
    double right = 0;
    if (reg(0x084) & 0x80) {
        const std::uint16_t cnt_l = reg(0x080);
        const std::uint16_t cnt_h = reg(0x082);
        for (unsigned i = 0; i < 4; ++i) {
            if (cnt_l & (0x1000u << i)) left += psg[i];
            if (cnt_l & (0x0100u << i)) right += psg[i];
        }
        left *= ((cnt_l >> 4) & 7) + 1;
        right *= (cnt_l & 7) + 1;
        static constexpr double kPsgVolume[4] = {0.25, 0.5, 1.0, 0.25};  // 3 is prohibited
        left *= kPsgVolume[cnt_h & 3];
        right *= kPsgVolume[cnt_h & 3];
        for (unsigned f = 0; f < 2; ++f) {
            const double sample = fifo_[f].current * ((cnt_h & (4u << f)) ? 4.0 : 2.0);
            if (cnt_h & (0x200u << (4 * f))) left += sample;
            if (cnt_h & (0x100u << (4 * f))) right += sample;
        }
    }
    const double bias = reg(0x088) & 0x3FEu;
    const auto to_pcm = [bias](double v) {
        const double dac = std::clamp(bias + v, 0.0, 1023.0);
        return static_cast<std::int16_t>(std::lround((dac - 512.0) * 64.0));
    };
    out_.push_back(to_pcm(left));
    out_.push_back(to_pcm(right));
    // Nobody is draining (no audio device, nothing capturing): keep the last second only.
    if (out_.size() > 4 * static_cast<std::size_t>(kSampleRate)) {
        out_.erase(out_.begin(), out_.begin() + 2 * kSampleRate);
    }
}

}  // namespace anygba
