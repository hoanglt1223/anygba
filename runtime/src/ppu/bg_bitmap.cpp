#include "ppu/ppu.h"

// Bitmap modes on BG2: mode 3 (240x160 direct colour), mode 4 (240x160 paletted, two
// pages), mode 5 (160x128 direct colour, two pages). They use BG2's rotation/scaling
// parameters; outside the bitmap is transparent.
namespace anygba {

void Ppu::render_bitmap_bg(unsigned mode, unsigned line) {
    const std::uint16_t cnt = reg(0x00C);
    const std::int32_t pa = sreg(0x020);
    const std::int32_t pb = sreg(0x022);
    const std::int32_t pc = sreg(0x024);
    const std::int32_t pd = sreg(0x026);
    const std::int32_t width = mode == 5 ? 160 : 240;
    const std::int32_t height = mode == 5 ? 128 : 160;
    const unsigned page = (mode != 3 && (reg(0x000) & 0x10)) ? 0xA000 : 0;
    const bool mosaic = (cnt & 0x40) != 0;
    const unsigned mos_h = (reg(0x04C) & 0xFu) + 1;
    const unsigned mos_v = ((reg(0x04C) >> 4) & 0xFu) + 1;
    const std::uint8_t* vram = mem_.vram.data();

    std::int32_t x0 = ref_x_[0];
    std::int32_t y0 = ref_y_[0];
    if (mosaic) {
        const auto back = static_cast<std::int32_t>(line % mos_v);
        x0 -= back * pb;
        y0 -= back * pd;
    }
    auto& out = bg_[2];
    for (unsigned x = 0; x < static_cast<unsigned>(kScreenWidth); ++x) {
        const auto sx = static_cast<std::int32_t>(mosaic ? x - x % mos_h : x);
        const std::int32_t tx = (x0 + pa * sx) >> 8;
        const std::int32_t ty = (y0 + pc * sx) >> 8;
        if (tx < 0 || ty < 0 || tx >= width || ty >= height) {
            continue;
        }
        const auto pixel = static_cast<unsigned>(ty * width + tx);
        if (mode == 4) {
            const std::uint8_t index = vram[page + pixel];
            if (index) {
                out[x] = bg_color(index);
            }
        } else {
            const unsigned addr = page + pixel * 2;
            out[x] = static_cast<std::uint16_t>((vram[addr] | (vram[addr + 1] << 8)) & 0x7FFF);
        }
    }
}

}  // namespace anygba
