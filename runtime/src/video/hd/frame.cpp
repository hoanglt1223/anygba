#include "video/hd/frame.h"

#include <cstring>

namespace anygba::video::hd {

void capture_line(const Memory& mem, const std::array<std::int32_t, 4>& reference, LineState& out) {
    std::memcpy(out.io.data(), mem.io.data(), kIoBytes);
    std::memcpy(out.palette.data(), mem.palette.data(), Memory::kPaletteSize);
    std::memcpy(out.oam.data(), mem.oam.data(), Memory::kOamSize);
    out.ref = reference;
    // Identifies the palette for memo tables within one frame; not part of any stored key.
    std::uint64_t h = 0x9E3779B97F4A7C15ull;
    for (std::size_t i = 0; i < out.palette.size(); i += 4) {
        std::uint64_t word;
        std::memcpy(&word, out.palette.data() + i, 8);
        h = (h ^ word) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    out.palette_hash = h;
}

bool Recorder::on_line(System& sys, unsigned line) {
    if (line == 0) versions_ = 0;
    // At most 255 copies (one per line is 160); a game rewriting VRAM on every line keeps its last copy.
    if (versions_ == 0 || (sys.vram_writes() != writes_seen_ && versions_ < 255)) {
        if (frame_.vram.size() <= versions_) frame_.vram.emplace_back();
        frame_.vram[versions_] = sys.memory().vram;
        writes_seen_ = sys.vram_writes();
        ++versions_;
    }
    LineState& ls = frame_.lines[line];
    capture_line(sys.memory(), sys.affine_reference(), ls);
    ls.vram_version = static_cast<std::uint8_t>(versions_ - 1);
    if (line != kScreenHeight - 1) return false;
    frame_.number = sys.frames() + 1;
    return true;
}

}  // namespace anygba::video::hd
