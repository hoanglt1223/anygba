#include "ppu/ppu.h"

#include <algorithm>

// Layer composition: windows select which layers and effects each pixel may use; the two
// top-most visible layers (by priority, OBJ before BG on a tie, lower BG number first)
// feed alpha blending or the brightness effects.
namespace anygba {

namespace {

constexpr unsigned kObj = 4;
constexpr unsigned kBackdrop = 5;

struct Span {
    unsigned lo, hi;
    bool contains(unsigned v) const { return v >= lo && v < hi; }
};

// WINxH/WINxV: X1/Y1 in the high byte, X2/Y2 (exclusive) in the low byte. Per GBATEK, an
// end past the screen or before the start means "to the edge of the screen".
Span window_span(std::uint16_t value, unsigned limit) {
    const unsigned lo = value >> 8;
    unsigned hi = value & 0xFFu;
    if (hi > limit || lo > hi) hi = limit;
    return {lo, hi};
}

std::uint16_t blend(std::uint16_t a, std::uint16_t b, unsigned eva, unsigned evb) {
    unsigned out = 0;
    for (unsigned shift = 0; shift < 15; shift += 5) {
        const unsigned c = (((a >> shift) & 31u) * eva + ((b >> shift) & 31u) * evb) >> 4;
        out |= std::min(c, 31u) << shift;
    }
    return static_cast<std::uint16_t>(out);
}

std::uint16_t brighten(std::uint16_t a, unsigned evy) {
    unsigned out = 0;
    for (unsigned shift = 0; shift < 15; shift += 5) {
        const unsigned c = (a >> shift) & 31u;
        out |= (c + (((31 - c) * evy) >> 4)) << shift;
    }
    return static_cast<std::uint16_t>(out);
}

std::uint16_t darken(std::uint16_t a, unsigned evy) {
    unsigned out = 0;
    for (unsigned shift = 0; shift < 15; shift += 5) {
        const unsigned c = (a >> shift) & 31u;
        out |= (c - ((c * evy) >> 4)) << shift;
    }
    return static_cast<std::uint16_t>(out);
}

}  // namespace

void Ppu::compose(unsigned line, std::uint16_t* out) {
    const std::uint16_t dispcnt = reg(0x000);
    const bool win0 = (dispcnt & 0x2000) != 0;
    const bool win1 = (dispcnt & 0x4000) != 0;
    const bool objwin = (dispcnt & 0x8000) != 0;
    const bool any_window = win0 || win1 || objwin;
    const std::uint16_t winin = reg(0x048);
    const std::uint16_t winout = reg(0x04A);
    const bool in_win0_v = win0 && window_span(reg(0x044), kScreenHeight).contains(line);
    const bool in_win1_v = win1 && window_span(reg(0x046), kScreenHeight).contains(line);
    const Span win0_h = window_span(reg(0x040), kScreenWidth);
    const Span win1_h = window_span(reg(0x042), kScreenWidth);

    const std::uint16_t bldcnt = reg(0x050);
    const unsigned effect = (bldcnt >> 6) & 3u;
    const unsigned eva = std::min(reg(0x052) & 0x1Fu, 16u);
    const unsigned evb = std::min((reg(0x052) >> 8) & 0x1Fu, 16u);
    const unsigned evy = std::min(reg(0x054) & 0x1Fu, 16u);
    unsigned bg_priority[4];
    for (unsigned bg = 0; bg < 4; ++bg) {
        bg_priority[bg] = reg(0x008 + 2 * bg) & 3u;
    }
    const std::uint16_t backdrop = bg_color(0);

    for (unsigned x = 0; x < static_cast<unsigned>(kScreenWidth); ++x) {
        unsigned enable = 0x3F;
        if (any_window) {
            if (in_win0_v && win0_h.contains(x)) {
                enable = winin & 0x3Fu;
            } else if (in_win1_v && win1_h.contains(x)) {
                enable = (winin >> 8) & 0x3Fu;
            } else if (objwin && obj_window_[x]) {
                enable = (winout >> 8) & 0x3Fu;
            } else {
                enable = winout & 0x3Fu;
            }
        }

        unsigned layer[2] = {kBackdrop, kBackdrop};
        std::uint16_t color[2] = {backdrop, backdrop};
        unsigned found = 0;
        const ObjPixel& obj = obj_[x];
        const bool obj_visible = (enable & 0x10) && obj.color != kTransparent;
        for (unsigned prio = 0; prio < 4 && found < 2; ++prio) {
            if (obj_visible && obj.priority == prio) {
                layer[found] = kObj;
                color[found] = obj.color;
                ++found;
            }
            for (unsigned bg = 0; bg < 4 && found < 2; ++bg) {
                if (bg_priority[bg] == prio && (enable & (1u << bg)) && bg_[bg][x] != kTransparent) {
                    layer[found] = bg;
                    color[found] = bg_[bg][x];
                    ++found;
                }
            }
        }

        std::uint16_t result = color[0];
        if (enable & 0x20) {
            const bool second_target = ((bldcnt >> (8 + layer[1])) & 1u) != 0;
            if (layer[0] == kObj && obj.semi && second_target) {
                result = blend(color[0], color[1], eva, evb);
            } else if ((bldcnt >> layer[0]) & 1u) {
                switch (effect) {
                case 1:
                    if (second_target) result = blend(color[0], color[1], eva, evb);
                    break;
                case 2: result = brighten(color[0], evy); break;
                case 3: result = darken(color[0], evy); break;
                default: break;
                }
            }
        }
        out[x] = result;
    }
}

}  // namespace anygba
