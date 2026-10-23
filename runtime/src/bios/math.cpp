#include "bios/bios.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <algorithm>

namespace anygba::bios {

namespace {

// ARM multiplies wrap at 32 bits; doing them in uint32 keeps C++ free of overflow UB.
std::int32_t mul(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}

// The BIOS evaluates arctan as an odd polynomial in 1.14 fixed point (Horner form).
// Returns the angle in 0x10000-per-turn units; *r1 and *r3 get the values the BIOS leaves.
std::int32_t arctan_core(std::int32_t tan, std::uint32_t* r1, std::uint32_t* r3) {
    static constexpr std::int32_t kCoeff[] = {0x390, 0x91C, 0xFB6, 0x16AA, 0x2081, 0x3651, 0xA2F9};
    const std::int32_t sq = -(mul(tan, tan) >> 14);
    std::int32_t acc = mul(0xA9, sq) >> 14;
    acc += kCoeff[0];
    for (std::size_t i = 1; i < std::size(kCoeff); ++i) {
        acc = (mul(acc, sq) >> 14) + kCoeff[i];
    }
    if (r1) *r1 = static_cast<std::uint32_t>(sq);
    if (r3) *r3 = static_cast<std::uint32_t>(acc);
    return mul(tan, acc) >> 16;
}

// BIOS Div truncates towards zero like C++ division.
std::int32_t bios_div(std::int32_t num, std::int32_t denom) {
    if (denom == -1 && num == std::numeric_limits<std::int32_t>::min()) {
        return num;
    }
    return denom == 0 ? 0 : num / denom;
}

}  // namespace

void div(System& sys, std::int32_t num, std::int32_t denom) {
    anygba_cpu& c = sys.cpu();
    if (denom == 0) {
        // The BIOS only returns for |num| <= 1; anything else spins forever on hardware.
        if (num > 1 || num < -1) {
            char msg[96];
            std::snprintf(msg, sizeof msg, "BIOS Div: %d / 0 hangs the real BIOS", num);
            sys.fail(msg);
        }
        c.r[0] = num < 0 ? 0xFFFFFFFFu : 1u;
        c.r[1] = static_cast<std::uint32_t>(num);
        c.r[3] = 1;
        spend(sys, 40);
        return;
    }
    if (denom == -1 && num == std::numeric_limits<std::int32_t>::min()) {
        c.r[0] = c.r[3] = 0x80000000u;
        c.r[1] = 0;
    } else {
        const std::int32_t q = num / denom;
        c.r[0] = static_cast<std::uint32_t>(q);
        c.r[1] = static_cast<std::uint32_t>(num % denom);
        c.r[3] = q < 0 ? 0u - static_cast<std::uint32_t>(q) : static_cast<std::uint32_t>(q);
    }
    // One shift-subtract step per quotient bit.
    const int steps = std::max(1, std::countl_zero(static_cast<std::uint32_t>(denom)) -
                                      std::countl_zero(static_cast<std::uint32_t>(num)));
    spend(sys, 11 + 13 * steps);
}

void sqrt(System& sys) {
    anygba_cpu& c = sys.cpu();
    std::uint32_t x = c.r[0];
    std::uint32_t root = 0;
    for (std::uint32_t bit = 1u << 30; bit != 0; bit >>= 2) {
        if (x >= root + bit) {
            x -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
    }
    c.r[0] = root;
    spend(sys, 50 + 30 * std::bit_width(c.r[0]));
}

void arctan(System& sys) {
    anygba_cpu& c = sys.cpu();
    c.r[0] = static_cast<std::uint32_t>(arctan_core(static_cast<std::int32_t>(c.r[0]), &c.r[1], &c.r[3]));
    spend(sys, 70);
}

void arctan2(System& sys) {
    anygba_cpu& c = sys.cpu();
    const auto x = static_cast<std::int32_t>(c.r[0]);
    const auto y = static_cast<std::int32_t>(c.r[1]);
    const std::int64_t wx = x;
    const std::int64_t wy = y;
    std::int32_t angle;
    // Reduce to the octant where |ratio| <= 1, then offset by the octant's base angle.
    if (y == 0) {
        angle = x >= 0 ? 0 : 0x8000;
    } else if (x == 0) {
        angle = y >= 0 ? 0x4000 : 0xC000;
    } else if (y >= 0) {
        if (x >= 0 && x >= y) {
            angle = arctan_core(bios_div(mul(y, 1 << 14), x), &c.r[1], nullptr);
        } else if (x < 0 && -wx >= wy) {
            angle = 0x8000 + arctan_core(bios_div(mul(y, 1 << 14), x), &c.r[1], nullptr);
        } else {
            angle = 0x4000 - arctan_core(bios_div(mul(x, 1 << 14), y), &c.r[1], nullptr);
        }
    } else {
        if (x <= 0 && -wx > -wy) {
            angle = 0x8000 + arctan_core(bios_div(mul(y, 1 << 14), x), &c.r[1], nullptr);
        } else if (x > 0 && wx >= -wy) {
            angle = 0x10000 + arctan_core(bios_div(mul(y, 1 << 14), x), &c.r[1], nullptr);
        } else {
            angle = 0xC000 - arctan_core(bios_div(mul(x, 1 << 14), y), &c.r[1], nullptr);
        }
    }
    c.r[0] = static_cast<std::uint32_t>(angle) & 0xFFFF;
    c.r[3] = 0x170;  // a BIOS address the real routine leaves behind
    spend(sys, 120);
}

void midi_key_to_freq(System& sys) {
    // freq = WaveData.freq / 2^((180 - key - fine/256) / 12)
    anygba_cpu& c = sys.cpu();
    const std::uint32_t base = sys.read32(c.r[0] + 4);
    const double exponent = (180.0 - static_cast<double>(c.r[1]) - static_cast<double>(c.r[2]) / 256.0) / 12.0;
    c.r[0] = static_cast<std::uint32_t>(static_cast<double>(base) / std::exp2(exponent));
    spend(sys, 80);
}

}  // namespace anygba::bios
