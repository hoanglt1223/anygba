#include "ppu/ppu.h"

// OBJ layer: 128 sprites from OAM, regular or affine, 4bpp or 8bpp, 1D or 2D tile mapping.
// Per pixel the opaque sprite pixel with the best priority wins, the lower OAM index on a
// tie. A line has a budget of OBJ render cycles (1210, or 954 with "HBlank interval
// free"); sprites past the budget are not drawn.
namespace anygba {

namespace {

constexpr std::uint8_t kSizes[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},  // square
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},  // horizontal
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},  // vertical
};

}  // namespace

void Ppu::render_sprites(unsigned line) {
    const std::uint16_t dispcnt = reg(0x000);
    const bool one_d = (dispcnt & 0x40) != 0;
    const bool bitmap_mode = (dispcnt & 7) >= 3;
    int budget = (dispcnt & 0x20) ? 954 : 1210;
    const unsigned mos_h = ((reg(0x04C) >> 8) & 0xFu) + 1;
    const unsigned mos_v = ((reg(0x04C) >> 12) & 0xFu) + 1;
    const std::uint8_t* vram = mem_.vram.data();
    const std::uint8_t* oam = mem_.oam.data();
    const auto oam16 = [oam](unsigned off) { return static_cast<std::uint16_t>(oam[off] | (oam[off + 1] << 8)); };

    for (unsigned n = 0; n < 128; ++n) {
        const std::uint16_t a0 = oam16(n * 8);
        const std::uint16_t a1 = oam16(n * 8 + 2);
        const std::uint16_t a2 = oam16(n * 8 + 4);
        const bool affine = (a0 & 0x100) != 0;
        if (!affine && (a0 & 0x200)) continue;  // disabled
        const unsigned obj_mode = (a0 >> 10) & 3;
        const unsigned shape = a0 >> 14;
        if (obj_mode == 3 || shape == 3) continue;
        const int w = kSizes[shape][a1 >> 14][0];
        const int h = kSizes[shape][a1 >> 14][1];
        const bool doubled = affine && (a0 & 0x200);
        const int bw = doubled ? 2 * w : w;
        const int bh = doubled ? 2 * h : h;
        const int top = a0 & 0xFF;
        const int dy = static_cast<int>((line - static_cast<unsigned>(top)) & 0xFF);
        if (dy >= bh) continue;
        int x = a1 & 0x1FF;
        if (x >= kScreenWidth) x -= 512;

        const int cost = affine ? 10 + 2 * bw : bw;
        if (cost > budget) break;
        budget -= cost;

        const unsigned tile = a2 & 0x3FF;
        if (bitmap_mode && tile < 512) continue;
        const auto priority = static_cast<std::uint8_t>((a2 >> 10) & 3);
        const unsigned palette = a2 >> 12;
        const bool color256 = (a0 & 0x2000) != 0;
        const bool mosaic = (a0 & 0x1000) != 0;

        int sample_dy = dy;
        if (mosaic) {
            sample_dy = static_cast<int>(((line - line % mos_v) - static_cast<unsigned>(top)) & 0xFF);
            if (sample_dy >= bh) sample_dy = 0;  // the mosaic block starts above the sprite
        }
        std::int32_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            const unsigned group = ((a1 >> 9) & 0x1Fu) * 32;
            pa = static_cast<std::int16_t>(oam16(group + 6));
            pb = static_cast<std::int16_t>(oam16(group + 14));
            pc = static_cast<std::int16_t>(oam16(group + 22));
            pd = static_cast<std::int16_t>(oam16(group + 30));
        }
        const unsigned row_tiles = one_d ? static_cast<unsigned>(w / 8) * (color256 ? 2 : 1) : 32;

        for (int sx = 0; sx < bw; ++sx) {
            const int screen_x = x + sx;
            if (screen_x < 0 || screen_x >= kScreenWidth) continue;
            int local = sx;
            if (mosaic) {
                local = (screen_x - screen_x % static_cast<int>(mos_h)) - x;
                if (local < 0) local = 0;
            }
            int tx, ty;
            if (affine) {
                const int lx = local - bw / 2;
                const int ly = sample_dy - bh / 2;
                tx = ((pa * lx + pb * ly) >> 8) + w / 2;
                ty = ((pc * lx + pd * ly) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - local : local;
                ty = (a1 & 0x2000) ? h - 1 - sample_dy : sample_dy;
            }
            const auto utx = static_cast<unsigned>(tx);
            const auto uty = static_cast<unsigned>(ty);
            std::uint16_t color;
            if (color256) {
                const unsigned base = one_d ? tile : (tile & ~1u);
                const unsigned t = base + (uty >> 3) * row_tiles + (utx >> 3) * 2;
                const unsigned addr = 0x10000 + ((t * 32 + (uty & 7) * 8 + (utx & 7)) & 0x7FFF);
                const std::uint8_t index = vram[addr];
                if (!index) continue;
                color = obj_color(index);
            } else {
                const unsigned t = tile + (uty >> 3) * row_tiles + (utx >> 3);
                const unsigned addr = 0x10000 + ((t * 32 + (uty & 7) * 4 + (utx & 7) / 2) & 0x7FFF);
                const unsigned index = (vram[addr] >> ((utx & 1) * 4)) & 0xF;
                if (!index) continue;
                color = obj_color(palette * 16 + index);
            }
            if (obj_mode == 2) {
                obj_window_[static_cast<std::size_t>(screen_x)] = true;
                continue;
            }
            ObjPixel& p = obj_[static_cast<std::size_t>(screen_x)];
            if (priority < p.priority) {
                p = {color, priority, obj_mode == 1};
            }
        }
    }
}

}  // namespace anygba
