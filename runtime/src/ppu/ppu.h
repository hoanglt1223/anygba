#pragma once

#include "anygba/system.hpp"

#include <array>
#include <cstdint>

// Scanline renderer (GBATEK "LCD"). Each visible line is rendered at the start of its
// HBlank from the current IO registers, VRAM, OAM and palette, so mid-frame register
// changes (HBlank DMA, VCOUNT IRQs) take effect on the next line.
namespace anygba {

namespace state {
class Archive;
}

class Ppu {
public:
    explicit Ppu(const Memory& mem) : mem_(mem) {}

    void reset();
    // Renders `line` (0-159) into `out` (240 RGB555 pixels), then advances the affine
    // reference points by PB/PD.
    void render_line(unsigned line, std::uint16_t* out);
    // Start of VBlank: the BG2/BG3 reference points are reloaded from their registers.
    void latch_reference_points();
    // A BG2X/BG2Y/BG3X/BG3Y register was written: it applies from the current line on.
    void reference_written(unsigned io_offset);
    // Internal affine reference points for save states (line buffers are rebuilt every line).
    void serialize(state::Archive& ar);
    // Internal BG2X, BG2Y, BG3X, BG3Y reference points the last rendered line started from.
    const std::array<std::int32_t, 4>& line_reference() const { return line_reference_; }

    static constexpr std::uint16_t kTransparent = 0x8000;

private:
    struct ObjPixel {
        std::uint16_t color = kTransparent;
        std::uint8_t priority = 4;
        bool semi = false;
    };

    std::uint16_t reg(std::uint32_t off) const;
    std::int16_t sreg(std::uint32_t off) const { return static_cast<std::int16_t>(reg(off)); }
    std::uint16_t bg_color(unsigned index) const;
    std::uint16_t obj_color(unsigned index) const;

    void render_text_bg(unsigned bg, unsigned line);
    void render_affine_bg(unsigned bg, unsigned line);
    void render_bitmap_bg(unsigned mode, unsigned line);
    void render_sprites(unsigned line);
    void compose(unsigned line, std::uint16_t* out);

    const Memory& mem_;
    std::array<std::array<std::uint16_t, kScreenWidth>, 4> bg_{};
    std::array<ObjPixel, kScreenWidth> obj_{};
    std::array<bool, kScreenWidth> obj_window_{};
    std::array<std::int32_t, 2> ref_x_{};  // internal BG2/BG3 reference points, 20.8 fixed
    std::array<std::int32_t, 2> ref_y_{};
    std::array<std::int32_t, 4> line_reference_{};
};

}  // namespace anygba
