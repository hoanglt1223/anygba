#include "ppu/ppu.h"

// Rotation/scaling backgrounds: BG2 in mode 1, BG2 and BG3 in mode 2. Each line starts at
// the internal reference point, which the PPU advances by PB/PD after every line.
namespace anygba {

void Ppu::render_affine_bg(unsigned bg, unsigned line) {
    const unsigned i = bg - 2;
    const std::uint16_t cnt = reg(0x008 + 2 * bg);
    const std::uint32_t params = 0x020 + 0x10 * i;
    const std::int32_t pa = sreg(params);
    const std::int32_t pb = sreg(params + 2);
    const std::int32_t pc = sreg(params + 4);
    const std::int32_t pd = sreg(params + 6);
    const unsigned size = 128u << ((cnt >> 14) & 3);
    const bool wrap = (cnt & 0x2000) != 0;
    const unsigned char_base = ((cnt >> 2) & 3u) * 0x4000;
    const unsigned screen_base = ((cnt >> 8) & 0x1Fu) * 0x800;
    const bool mosaic = (cnt & 0x40) != 0;
    const unsigned mos_h = (reg(0x04C) & 0xFu) + 1;
    const unsigned mos_v = ((reg(0x04C) >> 4) & 0xFu) + 1;
    const std::uint8_t* vram = mem_.vram.data();

    std::int32_t x0 = ref_x_[i];
    std::int32_t y0 = ref_y_[i];
    if (mosaic) {
        // Lines inside a mosaic block repeat the block's first line.
        const auto back = static_cast<std::int32_t>(line % mos_v);
        x0 -= back * pb;
        y0 -= back * pd;
    }
    auto& out = bg_[bg];
    const auto mask = static_cast<std::int32_t>(size - 1);
    for (unsigned x = 0; x < static_cast<unsigned>(kScreenWidth); ++x) {
        const auto sx = static_cast<std::int32_t>(mosaic ? x - x % mos_h : x);
        std::int32_t tx = (x0 + pa * sx) >> 8;
        std::int32_t ty = (y0 + pc * sx) >> 8;
        if (wrap) {
            tx &= mask;
            ty &= mask;
        } else if (tx < 0 || ty < 0 || tx > mask || ty > mask) {
            continue;
        }
        const unsigned map = screen_base + static_cast<unsigned>((ty >> 3) * static_cast<std::int32_t>(size >> 3) + (tx >> 3));
        if (map >= 0x10000) {
            continue;
        }
        const unsigned addr = char_base + vram[map] * 64u + static_cast<unsigned>((ty & 7) * 8 + (tx & 7));
        if (addr < 0x10000 && vram[addr]) {
            out[x] = bg_color(vram[addr]);
        }
    }
}

}  // namespace anygba
