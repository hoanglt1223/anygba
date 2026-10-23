#include "ppu/ppu.h"

// Text (tiled, scrolling) backgrounds: modes 0 and 1.
namespace anygba {

void Ppu::render_text_bg(unsigned bg, unsigned line) {
    const std::uint16_t cnt = reg(0x008 + 2 * bg);
    const unsigned hofs = reg(0x010 + 4 * bg) & 0x1FF;
    const unsigned vofs = reg(0x012 + 4 * bg) & 0x1FF;
    const unsigned char_base = ((cnt >> 2) & 3u) * 0x4000;
    const unsigned screen_base = ((cnt >> 8) & 0x1Fu) * 0x800;
    const bool color256 = (cnt & 0x80) != 0;
    const unsigned width = (cnt & 0x4000) ? 512 : 256;
    const unsigned height = (cnt & 0x8000) ? 512 : 256;
    const bool mosaic = (cnt & 0x40) != 0;
    const unsigned mos_h = (reg(0x04C) & 0xFu) + 1;
    const unsigned mos_v = ((reg(0x04C) >> 4) & 0xFu) + 1;
    const std::uint8_t* vram = mem_.vram.data();

    const unsigned y = mosaic ? line - line % mos_v : line;
    const unsigned py = (y + vofs) & (height - 1);
    auto& out = bg_[bg];
    for (unsigned x = 0; x < static_cast<unsigned>(kScreenWidth); ++x) {
        const unsigned sx = mosaic ? x - x % mos_h : x;
        const unsigned px = (sx + hofs) & (width - 1);
        const unsigned block = (px >> 8) + (py >> 8) * (width >> 8);
        const unsigned entry_addr = screen_base + block * 0x800 + ((py & 255) >> 3) * 64 + ((px & 255) >> 3) * 2;
        if (entry_addr >= 0x10000) {
            continue;
        }
        const unsigned entry = vram[entry_addr] | (vram[entry_addr + 1] << 8);
        unsigned tx = px & 7;
        unsigned ty = py & 7;
        if (entry & 0x400) tx = 7 - tx;
        if (entry & 0x800) ty = 7 - ty;
        const unsigned tile = entry & 0x3FF;
        if (color256) {
            const unsigned addr = char_base + tile * 64 + ty * 8 + tx;
            if (addr < 0x10000 && vram[addr]) {
                out[x] = bg_color(vram[addr]);
            }
        } else {
            const unsigned addr = char_base + tile * 32 + ty * 4 + tx / 2;
            if (addr < 0x10000) {
                const unsigned index = (vram[addr] >> ((tx & 1) * 4)) & 0xF;
                if (index) {
                    out[x] = bg_color((entry >> 12) * 16 + index);
                }
            }
        }
    }
}

}  // namespace anygba
