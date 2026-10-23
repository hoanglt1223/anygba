#include "apu/apu.h"

#include <algorithm>

// The four Game Boy channels (GBATEK "GBA Sound Channel 1-4"). Timers run in CPU cycles:
// a square duty step lasts (2048-n)*16 cycles, a wave digit (2048-n)*8, a noise step
// 64*r << s (32 << s for r = 0). Length, sweep and envelope are clocked by the 512 Hz
// frame sequencer. Each channel outputs +volume or -volume, so at full volume four
// channels span a quarter of the output range each, as GBATEK states.
namespace anygba {

namespace {

constexpr std::uint8_t kDuty[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},  // 12.5%
    {1, 0, 0, 0, 0, 0, 0, 1},  // 25%
    {1, 0, 0, 0, 0, 1, 1, 1},  // 50%
    {0, 1, 1, 1, 1, 1, 1, 0},  // 75%
};

bool dac_on(std::uint16_t envelope_reg) { return (envelope_reg & 0xF800) != 0; }

std::uint32_t noise_period(std::uint16_t reg) {
    const std::uint32_t r = reg & 7u;
    const std::uint32_t s = (reg >> 4) & 0xFu;
    return (r == 0 ? 32u : 64u * r) << s;
}

}  // namespace

void Apu::Envelope::load(std::uint16_t reg) {
    volume = reg >> 12;
    up = (reg & 0x0800) != 0;
    period = (reg >> 8) & 7u;
    timer = period;
}

void Apu::Envelope::clock() {
    if (period == 0 || timer == 0 || --timer != 0) {
        return;
    }
    timer = period;
    if (up && volume < 15) ++volume;
    if (!up && volume > 0) --volume;
}

void Apu::psg_write(std::uint32_t off, std::uint16_t value) {
    switch (off) {
    case 0x060: {
        Square& ch = square_[0];
        ch.sweep_shift = value & 7u;
        ch.sweep_down = (value & 8) != 0;
        ch.sweep_period = (value >> 4) & 7u;
        return;
    }
    case 0x062: case 0x068: {
        Square& ch = square_[off == 0x062 ? 0 : 1];
        ch.length = 64 - (value & 0x3Fu);
        ch.duty = (value >> 6) & 3u;
        if (!dac_on(value)) ch.on = false;
        return;
    }
    case 0x064: case 0x06C: {
        const int n = off == 0x064 ? 0 : 1;
        Square& ch = square_[n];
        ch.freq = value & 0x7FFu;
        ch.length_on = (value & 0x4000) != 0;
        if (value & 0x8000) trigger_square(n);
        return;
    }
    case 0x070:
        if (!(value & 0x80)) wave_.on = false;
        return;
    case 0x072:
        wave_.length = 256 - (value & 0xFFu);
        return;
    case 0x074:
        wave_.freq = value & 0x7FFu;
        wave_.length_on = (value & 0x4000) != 0;
        if (value & 0x8000) trigger_wave();
        return;
    case 0x078:
        noise_.length = 64 - (value & 0x3Fu);
        if (!dac_on(value)) noise_.on = false;
        return;
    case 0x07C:
        noise_.length_on = (value & 0x4000) != 0;
        if (value & 0x8000) trigger_noise();
        return;
    default:
        if (off >= 0x090 && off < 0x0A0) {
            // The CPU sees the bank that is not selected for playback.
            const unsigned bank = ((reg(0x070) >> 6) & 1u) ^ 1u;
            wave_.ram[bank][off - 0x090] = static_cast<std::uint8_t>(value);
            wave_.ram[bank][off - 0x090 + 1] = static_cast<std::uint8_t>(value >> 8);
        }
        return;
    }
}

void Apu::trigger_square(int n) {
    Square& ch = square_[static_cast<std::size_t>(n)];
    const std::uint16_t env = reg(n == 0 ? 0x062 : 0x068);
    ch.on = dac_on(env);
    if (ch.length == 0) ch.length = 64;
    ch.countdown = square_period(ch);
    ch.env.load(env);
    if (n == 0) {
        ch.shadow = ch.freq;
        ch.sweep_timer = ch.sweep_period ? ch.sweep_period : 8;
        ch.sweep_on = ch.sweep_period != 0 || ch.sweep_shift != 0;
        if (ch.sweep_shift && !ch.sweep_down && ch.shadow + (ch.shadow >> ch.sweep_shift) > 2047) {
            ch.on = false;
        }
    }
}

void Apu::trigger_wave() {
    wave_.on = (reg(0x070) & 0x80) != 0;
    if (wave_.length == 0) wave_.length = 256;
    wave_.position = 0;
    wave_.countdown = (2048 - wave_.freq) * 8;
}

void Apu::trigger_noise() {
    const std::uint16_t env = reg(0x078);
    noise_.on = dac_on(env);
    if (noise_.length == 0) noise_.length = 64;
    noise_.lfsr = (reg(0x07C) & 8) ? 0x40u : 0x4000u;
    noise_.countdown = noise_period(reg(0x07C));
    noise_.env.load(env);
}

void Apu::frame_sequencer_step() {
    const unsigned step = sequencer_step_;
    sequencer_step_ = (step + 1) & 7u;
    if ((step & 1) == 0) {
        const auto clock_length = [](bool& on, unsigned& length, bool length_on) {
            if (length_on && length > 0 && --length == 0) on = false;
        };
        clock_length(square_[0].on, square_[0].length, square_[0].length_on);
        clock_length(square_[1].on, square_[1].length, square_[1].length_on);
        clock_length(wave_.on, wave_.length, wave_.length_on);
        clock_length(noise_.on, noise_.length, noise_.length_on);
    }
    if (step == 2 || step == 6) {
        Square& ch = square_[0];
        if (ch.sweep_timer > 0 && --ch.sweep_timer == 0) {
            ch.sweep_timer = ch.sweep_period ? ch.sweep_period : 8;
            if (ch.sweep_on && ch.sweep_period) {
                const unsigned delta = ch.shadow >> ch.sweep_shift;
                const unsigned next = ch.sweep_down ? ch.shadow - delta : ch.shadow + delta;
                if (next > 2047) {
                    ch.on = false;
                } else if (ch.sweep_shift) {
                    ch.shadow = ch.freq = next;
                    if (!ch.sweep_down && next + (next >> ch.sweep_shift) > 2047) ch.on = false;
                }
            }
        }
    }
    if (step == 7) {
        square_[0].env.clock();
        square_[1].env.clock();
        noise_.env.clock();
    }
}

std::int32_t Apu::run_square(Square& ch, std::uint32_t cycles) {
    if (!ch.on) {
        return 0;
    }
    const auto volume = static_cast<std::int32_t>(ch.env.volume);
    std::int32_t acc = 0;
    while (cycles > 0) {
        const std::uint32_t take = std::min(cycles, ch.countdown);
        acc += (kDuty[ch.duty][ch.step] ? volume : -volume) * static_cast<std::int32_t>(take);
        ch.countdown -= take;
        cycles -= take;
        if (ch.countdown == 0) {
            ch.step = (ch.step + 1) & 7u;
            ch.countdown = square_period(ch);
        }
    }
    return acc;
}

std::int32_t Apu::run_wave(std::uint32_t cycles) {
    if (!wave_.on) {
        return 0;
    }
    const std::uint16_t cnt_l = reg(0x070);
    const std::uint16_t cnt_h = reg(0x072);
    const bool two_banks = (cnt_l & 0x20) != 0;
    const unsigned first_bank = (cnt_l >> 6) & 1u;
    // The volume code scales the 4-bit digit before the DAC: x1, x1/2, x1/4 (shifts) or x3/4,
    // so lower volumes also lose resolution. The level is centred on the scaled range.
    const unsigned code = (cnt_h >> 13) & 3u;
    const bool force75 = (cnt_h & 0x8000) != 0;
    if (code == 0 && !force75) {
        return 0;
    }
    const auto scale = [code, force75](std::int32_t digit) {
        return force75 ? (digit * 3) >> 2 : digit >> (code - 1);
    };
    const std::int32_t top = scale(15);
    std::int32_t acc = 0;
    while (cycles > 0) {
        const std::uint32_t take = std::min(cycles, wave_.countdown);
        const unsigned bank = two_banks ? (first_bank + wave_.position / 32) & 1u : first_bank;
        const unsigned index = wave_.position % 32;
        const std::uint8_t byte = wave_.ram[bank][index / 2];
        const auto digit = static_cast<std::int32_t>((index & 1) ? byte & 0xF : byte >> 4);
        acc += (2 * scale(digit) - top) * static_cast<std::int32_t>(take);
        wave_.countdown -= take;
        cycles -= take;
        if (wave_.countdown == 0) {
            wave_.position = (wave_.position + 1) % (two_banks ? 64u : 32u);
            wave_.countdown = (2048 - wave_.freq) * 8;
        }
    }
    return acc;
}

std::int32_t Apu::run_noise(std::uint32_t cycles) {
    if (!noise_.on) {
        return 0;
    }
    const std::uint16_t cnt = reg(0x07C);
    const std::uint32_t tap = (cnt & 8) ? 0x60u : 0x6000u;
    const auto volume = static_cast<std::int32_t>(noise_.env.volume);
    std::int32_t acc = 0;
    while (cycles > 0) {
        const std::uint32_t take = std::min(cycles, noise_.countdown);
        acc += (noise_.high ? volume : -volume) * static_cast<std::int32_t>(take);
        noise_.countdown -= take;
        cycles -= take;
        if (noise_.countdown == 0) {
            const bool carry = (noise_.lfsr & 1) != 0;
            noise_.lfsr >>= 1;
            if (carry) noise_.lfsr ^= tap;
            noise_.high = carry;
            noise_.countdown = noise_period(cnt);
        }
    }
    return acc;
}

}  // namespace anygba
