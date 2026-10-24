#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace anygba::state {
class Archive;
}

// Seiko S-3511 real-time clock on the cartridge GPIO port (GBATEK "GBA Cart Real-Time Clock").
// The port has three registers in ROM space: data at 0x080000C4 (bit 0 SCK, bit 1 SIO,
// bit 2 CS), direction at 0x080000C6 (1 = GBA drives the pin) and control at 0x080000C8
// (bit 0: the registers read back; otherwise the ROM bytes show through).
namespace anygba {

class Rtc {
public:
    // Broken-down local time the chip reports.
    struct Clock {
        int year = 2000;  // 2000-2099
        int month = 1;    // 1-12
        int day = 1;      // 1-31
        int weekday = 0;  // 0 = Sunday
        int hour = 0;
        int minute = 0;
        int second = 0;
    };

    explicit Rtc(std::function<Clock()> clock) : clock_(std::move(clock)) {}

    void reset();
    // offset 0xC4, 0xC6 or 0xC8 within the cartridge.
    void write(std::uint32_t offset, std::uint16_t value);
    bool readable() const { return (control_ & 1) != 0; }
    // The three port registers as the CPU reads them while readable().
    std::array<std::uint16_t, 3> registers() const { return {pins_, direction_, control_}; }

    void serialize(state::Archive& ar);

private:
    void clock_pins();
    void command_byte();
    unsigned output_bit() const;
    void latch_time();

    std::function<Clock()> clock_;
    std::uint16_t pins_ = 0;
    std::uint16_t direction_ = 0;
    std::uint16_t control_ = 0;

    std::uint8_t step_ = 0;       // 0 idle, 1 SCK high/CS low seen, 2 selected
    std::uint8_t bits_ = 0;       // byte being shifted in
    std::uint8_t bit_count_ = 0;  // bits of the current byte done
    std::uint8_t command_ = 0;    // active command byte, 0 when none
    std::uint8_t remaining_ = 0;  // bytes left in the active command
    std::uint8_t status_ = 0x40;  // status register; 0x40 = 24-hour mode, 0x80 = power lost
    std::array<std::uint8_t, 7> time_{};  // BCD year, month, day, weekday, hour, minute, second
};

}  // namespace anygba
