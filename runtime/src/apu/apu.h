#pragma once

#include "anygba/system.hpp"

#include <array>
#include <cstdint>
#include <vector>

// Sound (GBATEK "GBA Sound"). The four Game Boy PSG channels and the two DirectSound FIFOs
// are mixed at the GBA's 32768 Hz output rate into signed 16-bit stereo. State changes
// (register writes, FIFO pops on timer overflow) first bring the output up to the cycle
// they happen at, so sample timing does not depend on when the CPU happens to tick.
namespace anygba {

namespace state {
class Archive;
}

class Apu {
public:
    static constexpr int kSampleRate = 32768;
    static constexpr std::uint32_t kCyclesPerSample = 512;  // 16.78 MHz / 32768

    explicit Apu(System& sys) : sys_(sys) {}

    void reset();
    // Produces every output sample due at or before cycle `now`.
    void catch_up(std::uint64_t now);
    // A sound register (0x060-0x0A7) was written; the raw IO bytes already hold `value`.
    void write(std::uint32_t off, std::uint16_t value);
    // SOUNDCNT_X bits 0-3: which PSG channels are playing.
    std::uint16_t status() const;
    // Timer 0 or 1 overflowed at cycle `at`: FIFOs clocked by it play their next byte.
    void timer_overflow(int timer, std::uint64_t at);
    // Moves produced samples (interleaved L, R) to `out`.
    void drain(std::vector<std::int16_t>& out);
    // Timer overflows that found a FIFO empty after it had started playing.
    std::uint64_t fifo_underruns() const { return underruns_; }
    // Channel and FIFO state for save states. Undrained output is dropped on load.
    void serialize(state::Archive& ar);

private:
    struct Envelope {
        unsigned volume = 0;
        unsigned period = 0;
        bool up = false;
        unsigned timer = 0;
        void load(std::uint16_t reg);
        void clock();
    };

    struct Square {
        bool on = false;
        unsigned duty = 0;
        unsigned step = 0;          // position in the 8-step duty pattern
        std::uint32_t countdown = 0;  // cycles to the next step
        unsigned freq = 0;
        unsigned length = 0;
        bool length_on = false;
        Envelope env;
        // Sweep (channel 1 only)
        unsigned sweep_period = 0, sweep_shift = 0, sweep_timer = 0, shadow = 0;
        bool sweep_down = false, sweep_on = false;
    };

    struct Wave {
        bool on = false;
        unsigned freq = 0;
        unsigned position = 0;  // 0-63 nibbles
        std::uint32_t countdown = 0;
        unsigned length = 0;
        bool length_on = false;
        std::array<std::array<std::uint8_t, 16>, 2> ram{};
    };

    struct Noise {
        bool on = false;
        std::uint32_t lfsr = 0x4000;
        std::uint32_t countdown = 0;
        unsigned length = 0;
        bool length_on = false;
        bool high = false;
        Envelope env;
    };

    struct Fifo {
        std::array<std::int8_t, 32> data{};
        unsigned read = 0, size = 0;
        std::int8_t current = 0;
        bool fed = false;  // has received data since the last reset
        void push(std::uint8_t b);
        void clear() {
            read = size = 0;
            fed = false;
        }
    };

    std::uint16_t reg(std::uint32_t off) const;

    // psg.cpp
    void psg_write(std::uint32_t off, std::uint16_t value);
    void trigger_square(int n);
    void trigger_wave();
    void trigger_noise();
    void frame_sequencer_step();
    // Average output of each PSG channel over the next `cycles`, in units of 1/`cycles`
    // (signed, -15..15 per cycle); advances the channel timers.
    std::int32_t run_square(Square& ch, std::uint32_t cycles);
    std::int32_t run_wave(std::uint32_t cycles);
    std::int32_t run_noise(std::uint32_t cycles);
    std::uint32_t square_period(const Square& ch) const { return (2048 - ch.freq) * 16; }

    // mixer.cpp
    void produce_sample();

    System& sys_;
    std::array<Square, 2> square_{};
    Wave wave_{};
    Noise noise_{};
    std::array<Fifo, 2> fifo_{};
    unsigned sequencer_step_ = 0;
    unsigned samples_to_sequencer_ = 64;
    std::uint64_t next_sample_ = 0;
    std::vector<std::int16_t> out_;
    std::uint64_t underruns_ = 0;
};

}  // namespace anygba
