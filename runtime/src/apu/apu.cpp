#include "apu/apu.h"

#include <cstring>

namespace anygba {

std::uint16_t Apu::reg(std::uint32_t off) const {
    std::uint16_t v;
    std::memcpy(&v, sys_.memory().io.data() + off, 2);
    return v;
}

void Apu::reset() {
    square_ = {};
    wave_ = {};
    noise_ = {};
    fifo_ = {};
    underruns_ = 0;
    sequencer_step_ = 0;
    samples_to_sequencer_ = 1;  // the 512 Hz sequencer runs step 0 right after reset
    next_sample_ = kCyclesPerSample;
    out_.clear();
}

void Apu::catch_up(std::uint64_t now) {
    while (next_sample_ <= now) {
        produce_sample();
        next_sample_ += kCyclesPerSample;
        if (--samples_to_sequencer_ == 0) {
            samples_to_sequencer_ = 64;  // 32768 / 64 = 512 Hz
            frame_sequencer_step();
        }
    }
}

void Apu::write(std::uint32_t off, std::uint16_t value) {
    std::uint8_t* io = sys_.memory().io.data();
    switch (off) {
    case 0x082:
        if (value & 0x0800) fifo_[0].clear();
        if (value & 0x8000) fifo_[1].clear();
        io[0x083] &= 0x77;  // the reset bits read back as 0
        return;
    case 0x084:
        if (!(value & 0x80)) {
            // Master disable resets every PSG register and silences all channels.
            std::memset(io + 0x060, 0, 0x082 - 0x060);
            square_ = {};
            wave_.on = false;
            noise_ = {};
        }
        return;
    case 0x0A0: case 0x0A2: case 0x0A4: case 0x0A6: {
        Fifo& fifo = fifo_[off < 0x0A4 ? 0 : 1];
        fifo.push(static_cast<std::uint8_t>(value));
        fifo.push(static_cast<std::uint8_t>(value >> 8));
        return;
    }
    default:
        psg_write(off, value);
        return;
    }
}

std::uint16_t Apu::status() const {
    return static_cast<std::uint16_t>((square_[0].on ? 1 : 0) | (square_[1].on ? 2 : 0) | (wave_.on ? 4 : 0) |
                                      (noise_.on ? 8 : 0));
}

void Apu::drain(std::vector<std::int16_t>& out) {
    out.insert(out.end(), out_.begin(), out_.end());
    out_.clear();
}

}  // namespace anygba
