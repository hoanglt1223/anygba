#include "rtc.h"

#include "state/archive.h"

// Serial protocol as the GBA games drive it (GBATEK "S-3511"; mGBA's gpio.c behaves the same):
// a transfer starts with CS low and SCK high, then CS goes high. Each bit is presented on SIO
// while SCK is low and taken on the rising edge. The first byte is the command
// (low nibble 0110, bits 4-6 the register, bit 7 = read); data bytes follow, LSB first.
namespace anygba {

namespace {

constexpr std::uint8_t kSck = 1;
constexpr std::uint8_t kSio = 2;
constexpr std::uint8_t kCs = 4;

enum Command : std::uint8_t { kReset = 0, kDateTime = 2, kForceIrq = 3, kStatus = 4, kTime = 6 };
constexpr std::uint8_t kBytes[8] = {0, 0, 7, 0, 1, 0, 3, 0};

constexpr std::uint8_t bcd(int v) { return static_cast<std::uint8_t>(((v / 10) % 10) << 4 | (v % 10)); }

std::uint8_t register_of(std::uint8_t command) { return (command >> 4) & 7; }
bool is_read(std::uint8_t command) { return (command & 0x80) != 0; }

}  // namespace

void Rtc::reset() {
    pins_ = direction_ = control_ = 0;
    step_ = bits_ = bit_count_ = command_ = remaining_ = 0;
    status_ = 0x40;
    time_ = {};
}

void Rtc::write(std::uint32_t offset, std::uint16_t value) {
    switch (offset) {
    case 0xC4:
        pins_ = static_cast<std::uint16_t>((pins_ & ~direction_) | (value & direction_ & 0xF));
        clock_pins();
        break;
    case 0xC6: direction_ = value & 0xF; break;
    case 0xC8: control_ = value & 1; break;
    default: break;
    }
}

void Rtc::clock_pins() {
    const auto drive = [this](unsigned pins) {
        pins_ = static_cast<std::uint16_t>((pins_ & direction_) | (pins & ~direction_ & 0xF));
    };
    switch (step_) {
    case 0:
        if ((pins_ & (kSck | kCs)) == kSck) {
            step_ = 1;
        }
        break;
    case 1:
        if ((pins_ & (kSck | kCs)) == (kSck | kCs)) {
            step_ = 2;
        } else if ((pins_ & (kSck | kCs)) != kSck) {
            step_ = 0;
        }
        break;
    default:
        if (!(pins_ & kSck)) {
            bits_ = static_cast<std::uint8_t>((bits_ & ~(1u << bit_count_)) | (((pins_ & kSio) >> 1) << bit_count_));
        } else if (pins_ & kCs) {
            if (!is_read(command_)) {
                if (++bit_count_ == 8) {
                    command_byte();
                }
            } else {
                drive(kSck | kCs | (output_bit() << 1));
                if (++bit_count_ == 8) {
                    bit_count_ = 0;
                    if (remaining_ <= 1) {
                        remaining_ = 0;
                        command_ = 0;
                    } else {
                        --remaining_;
                    }
                }
            }
        } else {
            // CS dropped: the transfer is over.
            bit_count_ = 0;
            remaining_ = 0;
            command_ = 0;
            step_ = pins_ & kSck;
            drive(kSck);
        }
        break;
    }
}

void Rtc::command_byte() {
    if (command_ == 0) {
        if ((bits_ & 0x0F) == 0x06) {
            command_ = bits_;
            remaining_ = kBytes[register_of(bits_)];
            switch (register_of(bits_)) {
            case kReset:
                status_ = 0;
                break;
            case kDateTime:
            case kTime:
                latch_time();
                break;
            default:
                break;
            }
            if (remaining_ == 0) {
                command_ = 0;
            }
        }
        // Anything else is not a command; the chip ignores it.
    } else {
        if (register_of(command_) == kStatus) {
            status_ = bits_;
        }
        // Writes to the date/time registers are ignored: the host clock is the time source.
        if (--remaining_ == 0) {
            command_ = 0;
        }
    }
    bits_ = 0;
    bit_count_ = 0;
}

unsigned Rtc::output_bit() const {
    if (command_ == 0) {
        return 0;
    }
    std::uint8_t byte = 0;
    switch (register_of(command_)) {
    case kStatus: byte = status_; break;
    case kDateTime: byte = time_[7 - remaining_]; break;
    case kTime: byte = time_[7 - remaining_]; break;  // the last three: hour, minute, second
    default: break;
    }
    return (byte >> bit_count_) & 1u;
}

void Rtc::latch_time() {
    const Clock t = clock_();
    const bool h24 = (status_ & 0x40) != 0;
    time_[0] = bcd(t.year - 2000);
    time_[1] = bcd(t.month);
    time_[2] = bcd(t.day);
    time_[3] = bcd(t.weekday);
    // Bit 7 of the hour is the PM flag in 12-hour mode.
    time_[4] = h24 ? bcd(t.hour) : static_cast<std::uint8_t>(bcd(t.hour % 12) | (t.hour >= 12 ? 0x80 : 0));
    time_[5] = bcd(t.minute);
    time_[6] = bcd(t.second);
}

void Rtc::serialize(state::Archive& ar) {
    ar.fields(pins_, direction_, control_, step_, bits_, bit_count_, command_, remaining_, status_);
    ar.pod(time_);
    if (ar.loading() && (bit_count_ > 8 || remaining_ > 7 || step_ > 2)) {
        throw Error("state file has an invalid real-time clock state");
    }
}

}  // namespace anygba
