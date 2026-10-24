#pragma once

#include "anygba/system.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// What the HD renderer needs from one frame of the game: the registers, palette, OAM and affine
// reference points of every line (copied as each line is drawn, so mid-frame changes such as
// HBlank palette or scroll writes are kept), and VRAM at the end of the frame.
namespace anygba::video::hd {

inline constexpr unsigned kIoBytes = 0x60;  // DISPCNT .. BLDY

struct LineState {
    std::array<std::uint8_t, kIoBytes> io{};
    std::array<std::uint16_t, 512> palette{};  // BG then OBJ, BGR555 as stored
    std::array<std::uint8_t, Memory::kOamSize> oam{};
    std::array<std::int32_t, 4> ref{};  // BG2X, BG2Y, BG3X, BG3Y at the start of the line
    std::uint64_t palette_hash = 0;     // identifies the palette contents within a run
    std::uint8_t vram_version = 0;      // index into Frame::vram

    std::uint16_t reg(unsigned off) const { return static_cast<std::uint16_t>(io[off] | (io[off + 1] << 8)); }
    std::int16_t sreg(unsigned off) const { return static_cast<std::int16_t>(reg(off)); }
    std::uint16_t oam16(unsigned off) const { return static_cast<std::uint16_t>(oam[off] | (oam[off + 1] << 8)); }
    std::uint16_t bg_color(unsigned i) const { return palette[i] & 0x7FFF; }
    std::uint16_t obj_color(unsigned i) const { return palette[256 + i] & 0x7FFF; }
};

void capture_line(const Memory& mem, const std::array<std::int32_t, 4>& reference, LineState& out);

// How a layer fills the columns a widescreen picture adds left and right of the GBA's 240.
enum class Extend : std::uint8_t {
    Map,     // from the layer's own tile map, bitmap or sprites (what the hardware would show)
    Clamp,   // repeat the edge column
    Mirror,  // reflect the picture at the edge
    Hide,    // nothing: the layer stays inside the middle 240 columns (HUDs, text boxes)
};

struct Scene {
    bool pillarbox = false;  // black bars instead of extra columns
    std::array<Extend, 5> layers{};  // BG0-BG3, then OBJ (Map or Hide)
};

// Tile map entries supplied for a text BG beyond what the game keeps in VRAM (a per-game
// widescreen provider), in unwrapped map tile coordinates: (screen x + HOFS) / 8 and
// (line + VOFS) / 8 without the wrap at the map size.
struct MapExtension {
    int x0 = 0, y0 = 0, width = 0, height = 0;
    std::vector<std::int32_t> entries;  // -1 where the provider has nothing

    bool empty() const { return width == 0 || height == 0; }
    std::optional<std::uint16_t> at(int tx, int ty) const {
        const int x = tx - x0, y = ty - y0;
        if (x < 0 || y < 0 || x >= width || y >= height) return std::nullopt;
        const std::int32_t e = entries[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                       static_cast<std::size_t>(x)];
        if (e < 0) return std::nullopt;
        return static_cast<std::uint16_t>(e);
    }
};

struct Frame {
    std::array<LineState, kScreenHeight> lines{};
    // VRAM as each line saw it: a new copy only when the game wrote VRAM since the last line,
    // so usually one per frame.
    std::vector<std::vector<std::uint8_t>> vram = {std::vector<std::uint8_t>(Memory::kVramSize)};
    std::uint64_t number = 0;  // frame counter of the game, for the dump index
    Scene scene;
    std::array<MapExtension, 4> extension;

    const std::uint8_t* vram_for(const LineState& ls) const { return vram[ls.vram_version].data(); }
};

// Fills a Frame line by line from a running system (System::on_line_rendered).
class Recorder {
public:
    // Returns true after the last visible line: the frame is complete.
    bool on_line(System& sys, unsigned line);
    Frame& frame() { return frame_; }
    const Frame& frame() const { return frame_; }

private:
    Frame frame_;
    std::size_t versions_ = 0;  // VRAM copies used this frame
    std::uint64_t writes_seen_ = 0;
};

}  // namespace anygba::video::hd
