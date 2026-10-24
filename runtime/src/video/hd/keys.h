#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Content keys of HD pack art (docs/user/HD-PACKS.md, "Keys"). They are part of the pack
// format: changing how a key is computed breaks every existing pack.
namespace anygba::video::hd {

enum class KeyType : std::uint8_t {
    Tile,         // "t":  an 8x8 tile as shown: its 64 colours, transparency included
    TileIndices,  // "ti": an 8x8 tile's colour indices only, so one image serves every palette
    Obj,          // "o":  a whole sprite (OBJ) as shown
    ObjIndices,   // "oi": a whole sprite's colour indices only
    Bitmap,       // "b":  a mode 3/4/5 bitmap frame as shown
};
inline constexpr int kKeyTypes = 5;

std::string_view prefix(KeyType type);
// "t-0123456789abcdef"
std::string key_name(KeyType type, std::uint64_t hash);
struct ParsedKey {
    KeyType type;
    std::uint64_t hash;
};
// Accepts "<prefix>-<16 hex digits>" (file stems of pack images).
std::optional<ParsedKey> parse_key(std::string_view name);

// 64-bit FNV-1a.
class Fnv {
public:
    void byte(std::uint8_t b) {
        h_ ^= b;
        h_ *= 0x100000001b3ull;
    }
    void u16(std::uint16_t v) {
        byte(static_cast<std::uint8_t>(v));
        byte(static_cast<std::uint8_t>(v >> 8));
    }
    std::uint64_t value() const { return h_; }

private:
    std::uint64_t h_ = 0xcbf29ce484222325ull;
};

inline constexpr std::uint16_t kClear = 0x8000;  // a transparent pixel (colour index 0)

// Pixels of a piece of art in its own orientation (no flips), row by row: BGR555 colours with
// kClear for transparent pixels, and the colour indices they came from.
struct Art {
    int width = 0;
    int height = 0;
    bool bpp8 = false;
    std::vector<std::uint16_t> colors;
    std::vector<std::uint8_t> indices;  // empty for direct-colour bitmaps
};

// Key of the art as shown (Tile, Obj or Bitmap by `type`) and of its indices (TileIndices or
// ObjIndices; 0 for bitmaps).
std::uint64_t color_key(KeyType type, const Art& art);
std::uint64_t index_key(KeyType type, const Art& art);

// An RGBA picture (0xAABBGGRR, alpha 0 = transparent) used as replacement art.
struct Texture {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
    std::uint32_t at(int x, int y) const {
        return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }
};

// 8 bits per channel from BGR555, by bit replication; kClear becomes transparent black.
inline std::uint32_t expand(std::uint16_t c) {
    if (c & kClear) return 0;
    const unsigned r = c & 31u, g = (c >> 5) & 31u, b = (c >> 10) & 31u;
    return ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
}

}  // namespace anygba::video::hd
