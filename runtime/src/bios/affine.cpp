#include "bios/bios.h"

#include <array>
#include <cmath>
#include <numbers>

// BgAffineSet / ObjAffineSet. The BIOS uses only the upper 8 bits of the angle and a
// 256-entry 1.14 sine table, so the matrix is computed in the same fixed point:
//   pa = sx*cos  pb = -(sx*sin)  pc = sy*sin  pd = sy*cos   (8.8 results, each product
// shifted right by 14 before pb is negated)
namespace anygba::bios {

namespace {

std::array<std::int16_t, 256> make_sine_table() {
    std::array<std::int16_t, 256> t{};
    for (std::size_t i = 0; i < t.size(); ++i) {
        t[i] = static_cast<std::int16_t>(std::lround(std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / 256.0) * 16384.0));
    }
    return t;
}

const std::array<std::int16_t, 256> kSine = make_sine_table();

struct Matrix {
    std::int32_t pa, pb, pc, pd;
};

Matrix rotate_scale(std::int32_t sx, std::int32_t sy, std::uint32_t angle) {
    const std::uint32_t theta = (angle >> 8) & 0xFF;
    const std::int32_t s = sine(theta);
    const std::int32_t co = sine(theta + 64);
    return {(sx * co) >> 14, -((sx * s) >> 14), (sy * s) >> 14, (sy * co) >> 14};
}

std::int32_t s16(std::uint32_t v) { return static_cast<std::int16_t>(v); }

}  // namespace

std::int32_t sine(std::uint32_t angle) { return kSine[angle & 0xFF]; }

void bg_affine_set(System& sys) {
    anygba_cpu& c = sys.cpu();
    std::uint32_t src = c.r[0];
    std::uint32_t dst = c.r[1];
    for (std::uint32_t n = c.r[2]; n > 0; --n) {
        const auto ox = static_cast<std::int32_t>(sys.read32(src));
        const auto oy = static_cast<std::int32_t>(sys.read32(src + 4));
        const std::int32_t cx = s16(sys.read16(src + 8));
        const std::int32_t cy = s16(sys.read16(src + 10));
        const Matrix m = rotate_scale(s16(sys.read16(src + 12)), s16(sys.read16(src + 14)), sys.read16(src + 16));
        sys.write16(dst, static_cast<std::uint32_t>(m.pa) & 0xFFFF);
        sys.write16(dst + 2, static_cast<std::uint32_t>(m.pb) & 0xFFFF);
        sys.write16(dst + 4, static_cast<std::uint32_t>(m.pc) & 0xFFFF);
        sys.write16(dst + 6, static_cast<std::uint32_t>(m.pd) & 0xFFFF);
        sys.write32(dst + 8, static_cast<std::uint32_t>(std::int64_t{ox} - (std::int64_t{m.pa} * cx + std::int64_t{m.pb} * cy)));
        sys.write32(dst + 12, static_cast<std::uint32_t>(std::int64_t{oy} - (std::int64_t{m.pc} * cx + std::int64_t{m.pd} * cy)));
        src += 20;
        dst += 16;
        spend(sys, 60);
    }
}

void obj_affine_set(System& sys) {
    anygba_cpu& c = sys.cpu();
    std::uint32_t src = c.r[0];
    std::uint32_t dst = c.r[1];
    const std::uint32_t stride = c.r[3];
    for (std::uint32_t n = c.r[2]; n > 0; --n) {
        const Matrix m = rotate_scale(s16(sys.read16(src)), s16(sys.read16(src + 2)), sys.read16(src + 4));
        sys.write16(dst, static_cast<std::uint32_t>(m.pa) & 0xFFFF);
        sys.write16(dst + stride, static_cast<std::uint32_t>(m.pb) & 0xFFFF);
        sys.write16(dst + 2 * stride, static_cast<std::uint32_t>(m.pc) & 0xFFFF);
        sys.write16(dst + 3 * stride, static_cast<std::uint32_t>(m.pd) & 0xFFFF);
        src += 8;
        dst += 4 * stride;
        spend(sys, 40);
    }
}

}  // namespace anygba::bios
