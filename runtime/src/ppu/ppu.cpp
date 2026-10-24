#include "ppu/ppu.h"

#include <algorithm>
#include <cstring>

namespace anygba {

namespace {

constexpr std::uint32_t kRefBase[2] = {0x028, 0x038};  // BG2X, BG3X; Y follows at +4

std::int32_t reference_from(const std::vector<std::uint8_t>& io, std::uint32_t off) {
    std::uint32_t v;
    std::memcpy(&v, io.data() + off, 4);
    return static_cast<std::int32_t>(v << 4) >> 4;  // 28-bit signed
}

}  // namespace

std::uint16_t Ppu::reg(std::uint32_t off) const {
    std::uint16_t v;
    std::memcpy(&v, mem_.io.data() + off, 2);
    return v;
}

std::uint16_t Ppu::bg_color(unsigned index) const {
    std::uint16_t v;
    std::memcpy(&v, mem_.palette.data() + index * 2, 2);
    return v & 0x7FFF;
}

std::uint16_t Ppu::obj_color(unsigned index) const { return bg_color(256 + index); }

void Ppu::reset() {
    for (auto& line : bg_) line.fill(kTransparent);
    obj_.fill({});
    obj_window_.fill(false);
    latch_reference_points();
}

void Ppu::latch_reference_points() {
    for (int i = 0; i < 2; ++i) {
        ref_x_[i] = reference_from(mem_.io, kRefBase[i]);
        ref_y_[i] = reference_from(mem_.io, kRefBase[i] + 4);
    }
}

void Ppu::reference_written(unsigned io_offset) {
    const unsigned off = io_offset & ~3u;
    for (int i = 0; i < 2; ++i) {
        if (off == kRefBase[i]) ref_x_[i] = reference_from(mem_.io, off);
        if (off == kRefBase[i] + 4) ref_y_[i] = reference_from(mem_.io, off);
    }
}

void Ppu::render_line(unsigned line, std::uint16_t* out) {
    line_reference_ = {ref_x_[0], ref_y_[0], ref_x_[1], ref_y_[1]};
    const std::uint16_t dispcnt = reg(0x000);
    if (dispcnt & 0x80) {
        std::fill(out, out + kScreenWidth, static_cast<std::uint16_t>(0x7FFF));  // forced blank: white
    } else {
        for (auto& l : bg_) l.fill(kTransparent);
        obj_.fill({});
        obj_window_.fill(false);
        const unsigned mode = dispcnt & 7;
        const auto enabled = [dispcnt](unsigned bg) { return (dispcnt >> (8 + bg)) & 1; };
        switch (mode) {
        case 0:
            for (unsigned bg = 0; bg < 4; ++bg) {
                if (enabled(bg)) render_text_bg(bg, line);
            }
            break;
        case 1:
            if (enabled(0)) render_text_bg(0, line);
            if (enabled(1)) render_text_bg(1, line);
            if (enabled(2)) render_affine_bg(2, line);
            break;
        case 2:
            if (enabled(2)) render_affine_bg(2, line);
            if (enabled(3)) render_affine_bg(3, line);
            break;
        case 3: case 4: case 5:
            if (enabled(2)) render_bitmap_bg(mode, line);
            break;
        default:
            break;  // modes 6 and 7 show only the backdrop
        }
        if (dispcnt & 0x1000) {
            render_sprites(line);
        }
        compose(line, out);
        if (reg(0x002) & 1) {
            // Green swap: each pixel pair exchanges its green components.
            for (int x = 0; x < kScreenWidth; x += 2) {
                const std::uint16_t a = out[x];
                const std::uint16_t b = out[x + 1];
                out[x] = static_cast<std::uint16_t>((a & ~0x03E0u) | (b & 0x03E0u));
                out[x + 1] = static_cast<std::uint16_t>((b & ~0x03E0u) | (a & 0x03E0u));
            }
        }
    }
    for (int i = 0; i < 2; ++i) {
        ref_x_[i] += sreg(kRefBase[i] - 6);  // PB
        ref_y_[i] += sreg(kRefBase[i] - 2);  // PD
    }
}

}  // namespace anygba
