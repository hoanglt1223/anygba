#pragma once

#include "anygba/system.hpp"
#include "ppu/ppu.h"
#include "video/hd/frame.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

// Smoothing for high-refresh displays (docs/user/VIDEO.md): pictures between two GBA frames,
// drawn by the PPU with background scroll, affine reference points and sprite positions moved
// part of the way from the previous frame to the current one. It only does so when the two
// frames differ by small motion alone; anything else (palette or register changes, sprites
// appearing, large jumps, bitmap modes) shows the real frame instead, so it never invents
// pixels the game would not draw.
namespace anygba::video {

class MotionInterpolator {
public:
    MotionInterpolator();

    // Records the state `line` was drawn with (System::on_line_rendered).
    void on_line(System& sys, unsigned line);
    // The frame being recorded is complete: compares it with the previous one.
    void end_frame();
    // Forgets the previous frame (after loading a state, or when smoothing is turned on).
    void reset();

    // True when pictures between the previous and the current frame can be drawn.
    bool smooth() const { return smooth_; }
    // The picture at `t` (0 < t < 1) of the way from the previous frame to the current one,
    // drawn with `vram` (the current frame's VRAM). Only valid while smooth().
    const std::array<std::uint16_t, kScreenWidth * kScreenHeight>& render(double t, std::span<const std::uint8_t> vram);

    // Largest per-frame move, in pixels, still interpolated.
    static constexpr int kMaxMove = 16;

private:
    bool compare() const;

    std::array<hd::LineState, kScreenHeight> lines_[2]{};
    int cur_ = 0;          // index of the frame being recorded
    bool have_prev_ = false;
    bool ended_ = false;   // the next line 0 starts a new frame
    bool smooth_ = false;
    Memory scratch_;
    std::unique_ptr<Ppu> ppu_;
    std::array<std::uint16_t, kScreenWidth * kScreenHeight> out_{};
};

// Signed difference b - a of two values that wrap at `range` (a power of two).
int wrapped_delta(int a, int b, int range);

}  // namespace anygba::video
