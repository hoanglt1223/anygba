#include "video/motion.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace anygba::video {

namespace {

constexpr unsigned kScrollFirst = 0x10, kScrollEnd = 0x20;  // BG0HOFS .. BG3VOFS
constexpr unsigned kRefOffsets[4] = {0x28, 0x2C, 0x38, 0x3C};  // BG2X, BG2Y, BG3X, BG3Y

bool is_reference(unsigned off) { return (off >= 0x28 && off < 0x30) || (off >= 0x38 && off < 0x40); }

bool obj_visible(std::uint16_t a0) { return (a0 & 0x100) || !(a0 & 0x200); }

int lerp(int a, int delta, double t) { return a + static_cast<int>(std::lround(delta * t)); }

}  // namespace

int wrapped_delta(int a, int b, int range) {
    int d = (b - a) & (range - 1);
    if (d >= range / 2) d -= range;
    return d;
}

MotionInterpolator::MotionInterpolator() : ppu_(std::make_unique<Ppu>(scratch_)) {}

void MotionInterpolator::reset() {
    have_prev_ = false;
    smooth_ = false;
}

void MotionInterpolator::on_line(System& sys, unsigned line) {
    if (line >= kScreenHeight) return;
    if (line == 0 && ended_) {
        cur_ ^= 1;
        ended_ = false;
    }
    hd::capture_line(sys.memory(), sys.affine_reference(), lines_[cur_][line]);
}

void MotionInterpolator::end_frame() {
    smooth_ = have_prev_ && compare();
    have_prev_ = true;
    ended_ = true;
}

bool MotionInterpolator::compare() const {
    const auto& prev = lines_[cur_ ^ 1];
    const auto& cur = lines_[cur_];
    bool moved = false;
    for (unsigned l = 0; l < kScreenHeight; ++l) {
        const hd::LineState& p = prev[l];
        const hd::LineState& c = cur[l];
        if (p.palette_hash != c.palette_hash || p.palette != c.palette) return false;
        for (unsigned off = 0; off < hd::kIoBytes; ++off) {
            if ((off >= kScrollFirst && off < kScrollEnd) || is_reference(off)) continue;
            if (p.io[off] != c.io[off]) return false;
        }
        const std::uint16_t dispcnt = c.reg(0x000);
        if ((dispcnt & 7) >= 3 || (dispcnt & 0x80)) return false;  // bitmaps and forced blank
        for (unsigned off = kScrollFirst; off < kScrollEnd; off += 2) {
            const int d = wrapped_delta(p.reg(off) & 0x1FF, c.reg(off) & 0x1FF, 512);
            if (std::abs(d) > kMaxMove) return false;
            moved |= d != 0;
        }
        for (int i = 0; i < 4; ++i) {
            const std::int64_t d = static_cast<std::int64_t>(c.ref[i]) - p.ref[i];
            if (std::llabs(d) > kMaxMove * 256) return false;
            moved |= d != 0;
        }
        for (unsigned n = 0; n < 128; ++n) {
            const std::uint16_t p0 = p.oam16(n * 8), c0 = c.oam16(n * 8);
            const std::uint16_t p1 = p.oam16(n * 8 + 2), c1 = c.oam16(n * 8 + 2);
            if (obj_visible(p0) != obj_visible(c0)) return false;
            if (!obj_visible(c0)) continue;
            // Everything but the position must match; the tile number may change (animation).
            if ((p0 & 0xFF00) != (c0 & 0xFF00) || (p1 & 0xFE00) != (c1 & 0xFE00)) return false;
            const int dy = wrapped_delta(p0 & 0xFF, c0 & 0xFF, 256);
            const int dx = wrapped_delta(p1 & 0x1FF, c1 & 0x1FF, 512);
            if (std::abs(dx) > kMaxMove || std::abs(dy) > kMaxMove) return false;
            moved |= dx != 0 || dy != 0;
        }
    }
    return moved;
}

const std::array<std::uint16_t, kScreenWidth * kScreenHeight>& MotionInterpolator::render(
    double t, std::span<const std::uint8_t> vram) {
    std::memcpy(scratch_.vram.data(), vram.data(), std::min<std::size_t>(vram.size(), scratch_.vram.size()));
    const auto& prev = lines_[cur_ ^ 1];
    const auto& cur = lines_[cur_];
    for (unsigned l = 0; l < kScreenHeight; ++l) {
        const hd::LineState& p = prev[l];
        const hd::LineState& c = cur[l];
        std::memcpy(scratch_.io.data(), c.io.data(), hd::kIoBytes);
        std::memcpy(scratch_.palette.data(), c.palette.data(), Memory::kPaletteSize);
        std::memcpy(scratch_.oam.data(), c.oam.data(), Memory::kOamSize);
        const auto put16 = [](std::uint8_t* at, unsigned value) {
            at[0] = static_cast<std::uint8_t>(value);
            at[1] = static_cast<std::uint8_t>(value >> 8);
        };
        for (unsigned off = kScrollFirst; off < kScrollEnd; off += 2) {
            const int a = p.reg(off) & 0x1FF;
            put16(scratch_.io.data() + off, static_cast<unsigned>(lerp(a, wrapped_delta(a, c.reg(off) & 0x1FF, 512), t)) & 0x1FF);
        }
        for (int i = 0; i < 4; ++i) {
            const std::int64_t d = static_cast<std::int64_t>(c.ref[i]) - p.ref[i];
            const auto v = static_cast<std::uint32_t>(p.ref[i] + static_cast<std::int32_t>(std::llround(static_cast<double>(d) * t)));
            std::memcpy(scratch_.io.data() + kRefOffsets[i], &v, 4);
            ppu_->reference_written(kRefOffsets[i]);
        }
        for (unsigned n = 0; n < 128; ++n) {
            const std::uint16_t c0 = c.oam16(n * 8), c1 = c.oam16(n * 8 + 2);
            if (!obj_visible(c0)) continue;
            const int py = p.oam16(n * 8) & 0xFF, px = p.oam16(n * 8 + 2) & 0x1FF;
            const int y = lerp(py, wrapped_delta(py, c0 & 0xFF, 256), t) & 0xFF;
            const int x = lerp(px, wrapped_delta(px, c1 & 0x1FF, 512), t) & 0x1FF;
            put16(scratch_.oam.data() + n * 8, (c0 & 0xFF00u) | static_cast<unsigned>(y));
            put16(scratch_.oam.data() + n * 8 + 2, (c1 & 0xFE00u) | static_cast<unsigned>(x));
        }
        ppu_->render_line(l, out_.data() + static_cast<std::size_t>(l) * kScreenWidth);
    }
    return out_;
}

}  // namespace anygba::video
