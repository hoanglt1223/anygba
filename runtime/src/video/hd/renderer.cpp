#include "video/hd/renderer.h"

#include "video/hd/layer_scale.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace anygba::video::hd {

namespace {

constexpr int kW = kScreenWidth;
constexpr int kH = kScreenHeight;
constexpr unsigned kObj = 4;
constexpr unsigned kBackdrop = 5;

constexpr std::uint8_t kSizes[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

const char* const kLayerNames[5] = {"bg0", "bg1", "bg2", "bg3", "obj"};

constexpr std::uint32_t kClearRgba = 0;
constexpr std::uint32_t kBlack = 0xFF000000u;
constexpr std::uint32_t kWhite = 0xFFFFFFFFu;

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int floor_mod(int a, int b) { return a - floor_div(a, b) * b; }
std::int64_t floor_div64(std::int64_t a, std::int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

bool opaque(std::uint32_t c) { return (c >> 24) >= 128; }

// Colour effects on 8-bit channels. Channels that are exact 5-bit expansions are computed in 5
// bits like the GBA, so frames without replacement art match the 1x PPU bit for bit.
unsigned expand5(unsigned c5) { return (c5 << 3) | (c5 >> 2); }
bool exact5(unsigned c8) { return c8 == expand5(c8 >> 3); }

std::uint32_t blend8(std::uint32_t a, std::uint32_t b, unsigned eva, unsigned evb) {
    std::uint32_t out = 0xFF000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const unsigned ca = (a >> shift) & 255u, cb = (b >> shift) & 255u;
        unsigned c;
        if (exact5(ca) && exact5(cb)) {
            c = expand5(std::min(((ca >> 3) * eva + (cb >> 3) * evb) >> 4, 31u));
        } else {
            c = std::min((ca * eva + cb * evb) >> 4, 255u);
        }
        out |= c << shift;
    }
    return out;
}

std::uint32_t brighten8(std::uint32_t a, unsigned evy) {
    std::uint32_t out = 0xFF000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const unsigned c = (a >> shift) & 255u;
        const unsigned r = exact5(c) ? expand5((c >> 3) + (((31 - (c >> 3)) * evy) >> 4)) : c + (((255 - c) * evy) >> 4);
        out |= r << shift;
    }
    return out;
}

std::uint32_t darken8(std::uint32_t a, unsigned evy) {
    std::uint32_t out = 0xFF000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const unsigned c = (a >> shift) & 255u;
        const unsigned r = exact5(c) ? expand5((c >> 3) - (((c >> 3) * evy) >> 4)) : c - ((c * evy) >> 4);
        out |= r << shift;
    }
    return out;
}

struct Span {
    int lo, hi;
    bool contains(int v) const { return v >= lo && v < hi; }
};

Span window_span(std::uint16_t value, int limit, int extra) {
    const int lo = value >> 8;
    int hi = value & 0xFF;
    if (hi > limit || lo > hi) hi = limit;
    // A window over the whole width also covers the widescreen columns.
    if (extra > 0 && lo == 0 && hi == limit) return {-extra, limit + extra};
    return {lo, hi};
}

// Where a layer samples screen column x (which may lie in the widescreen columns): the column
// itself, a stand-in column, or nothing.
bool source_column(Extend mode, int x, int& out) {
    if (x >= 0 && x < kW) {
        out = x;
        return true;
    }
    switch (mode) {
    case Extend::Map: out = x; return true;
    case Extend::Clamp: out = std::clamp(x, 0, kW - 1); return true;
    case Extend::Mirror: out = x < 0 ? -x - 1 : 2 * kW - 1 - x; out = std::clamp(out, 0, kW - 1); return true;
    case Extend::Hide: return false;
    }
    return false;
}

// Art of an 8x8 BG tile (char data in BG VRAM; bytes past 0x10000 read as colour 0).
void decode_bg_tile(const std::uint8_t* vram, unsigned addr, bool bpp8, unsigned bank, const LineState& ls, Art& art) {
    art.width = art.height = 8;
    art.bpp8 = bpp8;
    art.colors.resize(64);
    art.indices.resize(64);
    for (unsigned p = 0; p < 64; ++p) {
        unsigned index;
        if (bpp8) {
            const unsigned a = addr + p;
            index = a < 0x10000 ? vram[a] : 0;
        } else {
            const unsigned a = addr + p / 2;
            index = a < 0x10000 ? (vram[a] >> ((p & 1) * 4)) & 0xF : 0;
        }
        art.indices[p] = static_cast<std::uint8_t>(index);
        art.colors[p] = index ? ls.bg_color(bpp8 ? index : bank * 16 + index) : kClear;
    }
}

struct TileInfo {
    const Texture* texture = nullptr;
};

// A tile of a layer pixel with replacement art: where in the art the pixel is.
struct TexRef {
    const Texture* texture = nullptr;
    std::uint16_t tx = 0, ty = 0;  // texel in the art at 1x (flips applied)
    std::uint8_t flags = 0;        // kHFlip, kVFlip, kMosaic
};
constexpr std::uint8_t kHFlip = 1, kVFlip = 2, kMosaic = 4;

// A sprite with its decoded art and replacement art.
struct SpriteInfo {
    Art art;
    const Texture* obj_texture = nullptr;
    std::vector<const Texture*> tile_textures;  // per 8x8 tile of the sprite, row by row
    bool any_texture = false;
};

struct ObjAttr {
    std::uint8_t prio = 4;  // 4: no sprite
    std::uint8_t index = 0;
    bool semi = false;
};

struct SpecialSprite {
    std::uint8_t n = 0;
    const SpriteInfo* info = nullptr;
};

// Per-worker memo of tiles and sprites already looked up this frame.
struct Worker {
    std::unordered_map<std::uint64_t, TileInfo> tiles;
    std::unordered_map<std::uint64_t, const SpriteInfo*> sprites;
    std::deque<SpriteInfo> sprite_store;
    std::unordered_map<std::uint64_t, const Texture*> bitmaps;
    Art scratch;
};

std::uint64_t mix(std::uint64_t a, std::uint64_t b) {
    std::uint64_t h = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2));
    h ^= h >> 31;
    return h * 0xBF58476D1CE4E5B9ull;
}

// Memo key part for what a line's tiles look like: its palette and its VRAM copy.
std::uint64_t line_key(const LineState& ls) { return mix(ls.palette_hash, ls.vram_version); }

std::uint32_t sample(const Texture& t, int u, int v) { return t.at(std::clamp(u, 0, t.width - 1), std::clamp(v, 0, t.height - 1)); }

}  // namespace

struct Renderer::Impl {
    const Frame* frame = nullptr;
    RenderConfig cfg;
    int W = kW;       // picture width at 1x
    int E = 0;        // extra columns per side
    int N = 1;
    bool lookups = false;      // replacement art or a sink: tiles and sprites must be identified
    bool tile_art = false;     // the source has tile art
    bool obj_art = false;      // the source has whole-sprite art
    bool bitmap_art = false;

    // Pass 1 (1x) results, W x 160.
    std::array<std::vector<std::uint16_t>, 5> color;  // BGR555, kClear = transparent
    std::array<std::vector<TexRef>, 4> tex;
    std::vector<ObjAttr> obj_attr;
    std::vector<std::uint8_t> obj_window;
    std::array<std::vector<SpecialSprite>, kH> special;
    std::array<std::array<bool, 4>, kH> smooth_bg{};  // affine/bitmap layers drawn per sub-pixel
    std::array<std::array<bool, 4>, kH> bg_on{};

    // Per-layer scaled pictures (layer filter), W*N x 160*N, or empty.
    std::array<Image, 5> scaled;
    std::array<bool, 5> use_scaled{};

    std::mutex workers_mutex;
    std::vector<std::unique_ptr<Worker>> workers;

    struct Pending {
        std::uint64_t key;
        unsigned layer;
        int x, y;
    };
    std::mutex pending_mutex;
    std::vector<Pending> pending;

    Worker& new_worker() {
        std::lock_guard lock(workers_mutex);
        workers.push_back(std::make_unique<Worker>());
        return *workers.back();
    }

    std::size_t at(int xi, unsigned line) const { return static_cast<std::size_t>(line) * W + static_cast<std::size_t>(xi); }

    // ---- replacement art lookups ----

    TileInfo tile_info(Worker& w, const LineState& ls, unsigned addr, bool bpp8, unsigned bank, unsigned layer,
                       std::uint64_t* added = nullptr) {
        const std::uint64_t memo = mix(line_key(ls), addr | (bpp8 ? 1u << 20 : 0u) | (bank << 21));
        if (const auto it = w.tiles.find(memo); it != w.tiles.end()) return it->second;
        TileInfo info;
        decode_bg_tile(frame->vram_for(ls), addr, bpp8, bank, ls, w.scratch);
        const std::uint64_t ck = color_key(KeyType::Tile, w.scratch);
        const std::uint64_t ik = index_key(KeyType::TileIndices, w.scratch);
        if (tile_art) {
            info.texture = cfg.textures->find(KeyType::Tile, ck, 8, 8, N);
            if (!info.texture) info.texture = cfg.textures->find(KeyType::TileIndices, ik, 8, 8, N);
        }
        if (cfg.sink && cfg.sink->add(KeyType::Tile, ck, ik, w.scratch, kLayerNames[layer], frame->number) && added) {
            *added = ck;
        }
        w.tiles.emplace(memo, info);
        return info;
    }

    const Texture* bitmap_texture(Worker& w, const LineState& ls, unsigned mode, unsigned page) {
        const std::uint64_t memo = mix(line_key(ls), mode | (page << 4));
        if (const auto it = w.bitmaps.find(memo); it != w.bitmaps.end()) return it->second;
        const int bw = mode == 5 ? 160 : 240, bh = mode == 5 ? 128 : 160;
        Art art;
        art.width = bw;
        art.height = bh;
        art.colors.resize(static_cast<std::size_t>(bw) * bh);
        const std::uint8_t* vram = frame->vram_for(ls);
        for (int p = 0; p < bw * bh; ++p) {
            if (mode == 4) {
                const std::uint8_t index = vram[page + static_cast<unsigned>(p)];
                art.colors[static_cast<std::size_t>(p)] = index ? ls.bg_color(index) : kClear;
            } else {
                const unsigned a = page + static_cast<unsigned>(p) * 2;
                art.colors[static_cast<std::size_t>(p)] = static_cast<std::uint16_t>((vram[a] | (vram[a + 1] << 8)) & 0x7FFF);
            }
        }
        const std::uint64_t ck = color_key(KeyType::Bitmap, art);
        const Texture* t = bitmap_art ? cfg.textures->find(KeyType::Bitmap, ck, bw, bh, N) : nullptr;
        if (cfg.sink) cfg.sink->add(KeyType::Bitmap, ck, 0, art, "bg2", frame->number);
        w.bitmaps.emplace(memo, t);
        return t;
    }

    const SpriteInfo* sprite_info(Worker& w, const LineState& ls, unsigned n, bool one_d) {
        const std::uint16_t a0 = ls.oam16(n * 8), a1 = ls.oam16(n * 8 + 2), a2 = ls.oam16(n * 8 + 4);
        const unsigned shape = a0 >> 14;
        const bool color256 = (a0 & 0x2000) != 0;
        const std::uint64_t sig = (a0 & 0xE000u) | (static_cast<std::uint64_t>(a1 >> 14) << 16) |
                                  (static_cast<std::uint64_t>(color256 ? (a2 & 0x3FF) : a2 & 0xF3FF) << 18) |
                                  (static_cast<std::uint64_t>(one_d) << 34);
        const std::uint64_t memo = mix(line_key(ls), sig);
        if (const auto it = w.sprites.find(memo); it != w.sprites.end()) return it->second;
        SpriteInfo& s = w.sprite_store.emplace_back();
        const int sw = kSizes[shape][a1 >> 14][0];
        const int sh = kSizes[shape][a1 >> 14][1];
        const unsigned tile = a2 & 0x3FF;
        const unsigned palette = a2 >> 12;
        const unsigned row_tiles = one_d ? static_cast<unsigned>(sw / 8) * (color256 ? 2 : 1) : 32;
        const std::uint8_t* vram = frame->vram_for(ls);
        Art& art = s.art;
        art.width = sw;
        art.height = sh;
        art.bpp8 = color256;
        art.colors.resize(static_cast<std::size_t>(sw) * sh);
        art.indices.resize(static_cast<std::size_t>(sw) * sh);
        for (int y = 0; y < sh; ++y) {
            for (int x = 0; x < sw; ++x) {
                const auto ux = static_cast<unsigned>(x), uy = static_cast<unsigned>(y);
                unsigned index;
                if (color256) {
                    const unsigned base = one_d ? tile : (tile & ~1u);
                    const unsigned t = base + (uy >> 3) * row_tiles + (ux >> 3) * 2;
                    index = vram[0x10000 + ((t * 32 + (uy & 7) * 8 + (ux & 7)) & 0x7FFF)];
                } else {
                    const unsigned t = tile + (uy >> 3) * row_tiles + (ux >> 3);
                    index = (vram[0x10000 + ((t * 32 + (uy & 7) * 4 + (ux & 7) / 2) & 0x7FFF)] >> ((ux & 1) * 4)) & 0xF;
                }
                const std::size_t p = static_cast<std::size_t>(y) * sw + static_cast<std::size_t>(x);
                art.indices[p] = static_cast<std::uint8_t>(index);
                art.colors[p] = index ? ls.obj_color(color256 ? index : palette * 16 + index) : kClear;
            }
        }
        if (lookups) {
            const std::uint64_t ck = color_key(KeyType::Obj, art);
            const std::uint64_t ik = index_key(KeyType::ObjIndices, art);
            if (obj_art) {
                s.obj_texture = cfg.textures->find(KeyType::Obj, ck, sw, sh, N);
                if (!s.obj_texture) s.obj_texture = cfg.textures->find(KeyType::ObjIndices, ik, sw, sh, N);
            }
            if (tile_art && !s.obj_texture) {
                Art t;
                t.width = t.height = 8;
                t.bpp8 = color256;
                t.colors.resize(64);
                t.indices.resize(64);
                for (int ty = 0; ty < sh / 8; ++ty) {
                    for (int tx = 0; tx < sw / 8; ++tx) {
                        for (int p = 0; p < 64; ++p) {
                            const std::size_t src = static_cast<std::size_t>(ty * 8 + p / 8) * sw + static_cast<std::size_t>(tx * 8 + p % 8);
                            t.colors[static_cast<std::size_t>(p)] = art.colors[src];
                            t.indices[static_cast<std::size_t>(p)] = art.indices[src];
                        }
                        const Texture* tt = cfg.textures->find(KeyType::Tile, color_key(KeyType::Tile, t), 8, 8, N);
                        if (!tt) tt = cfg.textures->find(KeyType::TileIndices, index_key(KeyType::TileIndices, t), 8, 8, N);
                        s.tile_textures.push_back(tt);
                        s.any_texture |= tt != nullptr;
                    }
                }
            }
            s.any_texture |= s.obj_texture != nullptr;
            if (cfg.sink) cfg.sink->add(KeyType::Obj, ck, ik, art, "obj", frame->number);
        }
        w.sprites.emplace(memo, &s);
        return &s;
    }

    // ---- pass 1: one line of every layer at 1x ----

    void text_bg(Worker& wk, const LineState& ls, unsigned bg, unsigned line) {
        const std::uint16_t cnt = ls.reg(0x008 + 2 * bg);
        const int hofs = ls.reg(0x010 + 4 * bg) & 0x1FF;
        const int vofs = ls.reg(0x012 + 4 * bg) & 0x1FF;
        const unsigned char_base = ((cnt >> 2) & 3u) * 0x4000;
        const unsigned screen_base = ((cnt >> 8) & 0x1Fu) * 0x800;
        const bool color256 = (cnt & 0x80) != 0;
        const unsigned width = (cnt & 0x4000) ? 512 : 256;
        const unsigned height = (cnt & 0x8000) ? 512 : 256;
        const bool mosaic = (cnt & 0x40) != 0;
        const int mos_h = (ls.reg(0x04C) & 0xF) + 1;
        const unsigned mos_v = ((ls.reg(0x04C) >> 4) & 0xFu) + 1;
        const std::uint8_t* vram = frame->vram_for(ls);
        const Extend mode = frame->scene.layers[bg];
        const MapExtension& ext = frame->extension[bg];

        const unsigned y = mosaic ? line - line % mos_v : line;
        const unsigned py = (y + static_cast<unsigned>(vofs)) & (height - 1);
        std::uint16_t* out = color[bg].data() + at(0, line);
        TexRef* refs = lookups ? tex[bg].data() + at(0, line) : nullptr;
        for (int xi = 0; xi < W; ++xi) {
            int xs;
            if (!source_column(mode, xi - E, xs)) continue;
            const int sx = mosaic ? xs - floor_mod(xs, mos_h) : xs;
            unsigned entry;
            unsigned tx_s, ty_s;
            std::optional<std::uint16_t> extended;
            if ((xs < 0 || xs >= kW) && !ext.empty()) {
                extended = ext.at(floor_div(sx + hofs, 8), floor_div(static_cast<int>(y) + vofs, 8));
            }
            if (extended) {
                entry = *extended;
                tx_s = static_cast<unsigned>(floor_mod(sx + hofs, 8));
                ty_s = static_cast<unsigned>(floor_mod(static_cast<int>(y) + vofs, 8));
            } else {
                const unsigned px = static_cast<unsigned>(sx + hofs) & (width - 1);
                const unsigned block = (px >> 8) + (py >> 8) * (width >> 8);
                const unsigned entry_addr = screen_base + block * 0x800 + ((py & 255) >> 3) * 64 + ((px & 255) >> 3) * 2;
                if (entry_addr >= 0x10000) continue;
                entry = vram[entry_addr] | (vram[entry_addr + 1] << 8);
                tx_s = px & 7;
                ty_s = py & 7;
            }
            unsigned tx = tx_s, ty = ty_s;
            if (entry & 0x400) tx = 7 - tx;
            if (entry & 0x800) ty = 7 - ty;
            const unsigned tile = entry & 0x3FF;
            const unsigned bank = entry >> 12;
            unsigned addr;
            std::uint16_t c = kClear;
            if (color256) {
                addr = char_base + tile * 64;
                const unsigned a = addr + ty * 8 + tx;
                if (a < 0x10000 && vram[a]) c = ls.bg_color(vram[a]);
            } else {
                addr = char_base + tile * 32;
                const unsigned a = addr + ty * 4 + tx / 2;
                if (a < 0x10000) {
                    const unsigned index = (vram[a] >> ((tx & 1) * 4)) & 0xF;
                    if (index) c = ls.bg_color(bank * 16 + index);
                }
            }
            out[xi] = c;
            if (refs) {
                std::uint64_t added = 0;
                const TileInfo info = tile_info(wk, ls, addr, color256, color256 ? 0 : bank, bg, &added);
                if (added && !mosaic && !(entry & 0xC00) && xs >= 0 && xs < kW) {
                    std::lock_guard lock(pending_mutex);
                    pending.push_back({added, bg, xi - static_cast<int>(tx_s), static_cast<int>(line) - static_cast<int>(ty_s)});
                }
                if (info.texture) {
                    refs[xi] = {info.texture, static_cast<std::uint16_t>(tx), static_cast<std::uint16_t>(ty),
                                static_cast<std::uint8_t>(((entry & 0x400) ? kHFlip : 0) | ((entry & 0x800) ? kVFlip : 0) |
                                                          (mosaic ? kMosaic : 0))};
                }
            }
        }
    }

    void affine_bg(Worker& wk, const LineState& ls, unsigned bg, unsigned line) {
        const unsigned i = bg - 2;
        const std::uint16_t cnt = ls.reg(0x008 + 2 * bg);
        const std::uint32_t params = 0x020 + 0x10 * i;
        const std::int32_t pa = ls.sreg(params), pb = ls.sreg(params + 2), pc = ls.sreg(params + 4), pd = ls.sreg(params + 6);
        const unsigned size = 128u << ((cnt >> 14) & 3);
        const bool wrap = (cnt & 0x2000) != 0;
        const unsigned char_base = ((cnt >> 2) & 3u) * 0x4000;
        const unsigned screen_base = ((cnt >> 8) & 0x1Fu) * 0x800;
        const bool mosaic = (cnt & 0x40) != 0;
        const int mos_h = (ls.reg(0x04C) & 0xF) + 1;
        const unsigned mos_v = ((ls.reg(0x04C) >> 4) & 0xFu) + 1;
        const std::uint8_t* vram = frame->vram_for(ls);
        const Extend mode = frame->scene.layers[bg];

        std::int32_t x0 = ls.ref[2 * i], y0 = ls.ref[2 * i + 1];
        if (mosaic) {
            const auto back = static_cast<std::int32_t>(line % mos_v);
            x0 -= back * pb;
            y0 -= back * pd;
        }
        const auto mask = static_cast<std::int32_t>(size - 1);
        std::uint16_t* out = color[bg].data() + at(0, line);
        TexRef* refs = lookups ? tex[bg].data() + at(0, line) : nullptr;
        for (int xi = 0; xi < W; ++xi) {
            int xs;
            if (!source_column(mode, xi - E, xs)) continue;
            const std::int32_t sx = mosaic ? xs - floor_mod(xs, mos_h) : xs;
            std::int32_t tx = (x0 + pa * sx) >> 8;
            std::int32_t ty = (y0 + pc * sx) >> 8;
            if (wrap) {
                tx &= mask;
                ty &= mask;
            } else if (tx < 0 || ty < 0 || tx > mask || ty > mask) {
                continue;
            }
            const unsigned map = screen_base + static_cast<unsigned>((ty >> 3) * static_cast<std::int32_t>(size >> 3) + (tx >> 3));
            if (map >= 0x10000) continue;
            const unsigned addr = char_base + vram[map] * 64u;
            const unsigned a = addr + static_cast<unsigned>((ty & 7) * 8 + (tx & 7));
            if (a < 0x10000 && vram[a]) out[xi] = ls.bg_color(vram[a]);
            if (refs) {
                const TileInfo info = tile_info(wk, ls, addr, true, 0, bg);
                if (info.texture) {
                    refs[xi] = {info.texture, static_cast<std::uint16_t>(tx & 7), static_cast<std::uint16_t>(ty & 7),
                                static_cast<std::uint8_t>(mosaic ? kMosaic : 0)};
                }
            }
        }
    }

    void bitmap_bg(Worker& wk, const LineState& ls, unsigned mode, unsigned line) {
        const std::uint16_t cnt = ls.reg(0x00C);
        const std::int32_t pa = ls.sreg(0x020), pb = ls.sreg(0x022), pc = ls.sreg(0x024), pd = ls.sreg(0x026);
        const std::int32_t bw = mode == 5 ? 160 : 240, bh = mode == 5 ? 128 : 160;
        const unsigned page = (mode != 3 && (ls.reg(0x000) & 0x10)) ? 0xA000 : 0;
        const bool mosaic = (cnt & 0x40) != 0;
        const int mos_h = (ls.reg(0x04C) & 0xF) + 1;
        const unsigned mos_v = ((ls.reg(0x04C) >> 4) & 0xFu) + 1;
        const std::uint8_t* vram = frame->vram_for(ls);
        const Extend ext_mode = frame->scene.layers[2];

        std::int32_t x0 = ls.ref[0], y0 = ls.ref[1];
        if (mosaic) {
            const auto back = static_cast<std::int32_t>(line % mos_v);
            x0 -= back * pb;
            y0 -= back * pd;
        }
        const Texture* t = (bitmap_art || cfg.sink) ? bitmap_texture(wk, ls, mode, page) : nullptr;
        std::uint16_t* out = color[2].data() + at(0, line);
        TexRef* refs = lookups ? tex[2].data() + at(0, line) : nullptr;
        for (int xi = 0; xi < W; ++xi) {
            int xs;
            if (!source_column(ext_mode, xi - E, xs)) continue;
            const std::int32_t sx = mosaic ? xs - floor_mod(xs, mos_h) : xs;
            const std::int32_t tx = (x0 + pa * sx) >> 8;
            const std::int32_t ty = (y0 + pc * sx) >> 8;
            if (tx < 0 || ty < 0 || tx >= bw || ty >= bh) continue;
            const auto pixel = static_cast<unsigned>(ty * bw + tx);
            if (mode == 4) {
                const std::uint8_t index = vram[page + pixel];
                if (index) out[xi] = ls.bg_color(index);
            } else {
                const unsigned a = page + pixel * 2;
                out[xi] = static_cast<std::uint16_t>((vram[a] | (vram[a + 1] << 8)) & 0x7FFF);
            }
            if (refs && t) {
                refs[xi] = {t, static_cast<std::uint16_t>(tx), static_cast<std::uint16_t>(ty),
                            static_cast<std::uint8_t>(mosaic ? kMosaic : 0)};
            }
        }
    }

    // Screen column (relative to the 240-pixel screen) of sprite pixel `q` = (OAM x + offset)
    // mod 512, or false when it is outside the picture.
    bool sprite_column(int q, int& x) const {
        q &= 511;
        x = q < kW + E ? q : q - 512;
        if (x < -E || x >= kW + E) return false;
        if ((x < 0 || x >= kW) && frame->scene.layers[kObj] == Extend::Hide) return false;
        return true;
    }

    void sprites(Worker& wk, const LineState& ls, unsigned line) {
        const std::uint16_t dispcnt = ls.reg(0x000);
        const bool one_d = (dispcnt & 0x40) != 0;
        const bool bitmap_mode = (dispcnt & 7) >= 3;
        int budget = (dispcnt & 0x20) ? 954 : 1210;
        const unsigned mos_h = ((ls.reg(0x04C) >> 8) & 0xFu) + 1;
        const unsigned mos_v = ((ls.reg(0x04C) >> 12) & 0xFu) + 1;
        const std::uint8_t* vram = frame->vram_for(ls);
        std::uint16_t* out = color[kObj].data() + at(0, line);
        ObjAttr* attrs = obj_attr.data() + at(0, line);
        std::uint8_t* window = obj_window.data() + at(0, line);

        for (unsigned n = 0; n < 128; ++n) {
            const std::uint16_t a0 = ls.oam16(n * 8), a1 = ls.oam16(n * 8 + 2), a2 = ls.oam16(n * 8 + 4);
            const bool affine = (a0 & 0x100) != 0;
            if (!affine && (a0 & 0x200)) continue;
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
            const int xfield = a1 & 0x1FF;

            const int cost = affine ? 10 + 2 * bw : bw;
            if (cost > budget) break;
            budget -= cost;

            const unsigned tile = a2 & 0x3FF;
            if (bitmap_mode && tile < 512) continue;
            const auto priority = static_cast<std::uint8_t>((a2 >> 10) & 3);
            const unsigned palette = a2 >> 12;
            const bool color256 = (a0 & 0x2000) != 0;
            const bool mosaic = (a0 & 0x1000) != 0;

            const bool smooth = affine && cfg.smooth_affine && N > 1 && !mosaic;
            if (lookups || smooth) {
                const SpriteInfo* info = sprite_info(wk, ls, n, one_d);
                if (smooth || info->any_texture) {
                    special[line].push_back({static_cast<std::uint8_t>(n), info});
                    continue;  // drawn per sub-pixel in pass 2
                }
            }

            int sample_dy = dy;
            if (mosaic) {
                sample_dy = static_cast<int>(((line - line % mos_v) - static_cast<unsigned>(top)) & 0xFF);
                if (sample_dy >= bh) sample_dy = 0;
            }
            std::int32_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
            if (affine) {
                const unsigned group = ((a1 >> 9) & 0x1Fu) * 32;
                pa = static_cast<std::int16_t>(ls.oam16(group + 6));
                pb = static_cast<std::int16_t>(ls.oam16(group + 14));
                pc = static_cast<std::int16_t>(ls.oam16(group + 22));
                pd = static_cast<std::int16_t>(ls.oam16(group + 30));
            }
            const unsigned row_tiles = one_d ? static_cast<unsigned>(w / 8) * (color256 ? 2 : 1) : 32;
            for (int sx = 0; sx < bw; ++sx) {
                int screen_x;
                if (!sprite_column(xfield + sx, screen_x)) continue;
                const int x = screen_x - sx;  // sprite origin in screen columns
                int local = sx;
                if (mosaic) {
                    local = (screen_x - floor_mod(screen_x, static_cast<int>(mos_h))) - x;
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
                std::uint16_t c;
                if (color256) {
                    const unsigned base = one_d ? tile : (tile & ~1u);
                    const unsigned t = base + (uty >> 3) * row_tiles + (utx >> 3) * 2;
                    const std::uint8_t index = vram[0x10000 + ((t * 32 + (uty & 7) * 8 + (utx & 7)) & 0x7FFF)];
                    if (!index) continue;
                    c = ls.obj_color(index);
                } else {
                    const unsigned t = tile + (uty >> 3) * row_tiles + (utx >> 3);
                    const unsigned index = (vram[0x10000 + ((t * 32 + (uty & 7) * 4 + (utx & 7) / 2) & 0x7FFF)] >> ((utx & 1) * 4)) & 0xF;
                    if (!index) continue;
                    c = ls.obj_color(palette * 16 + index);
                }
                const int xi = screen_x + E;
                if (obj_mode == 2) {
                    window[xi] = 1;
                    continue;
                }
                ObjAttr& p = attrs[xi];
                if (priority < p.prio) {
                    p = {priority, static_cast<std::uint8_t>(n), obj_mode == 1};
                    out[xi] = c;
                }
            }
        }
    }

    void pass1_line(Worker& wk, unsigned line) {
        const LineState& ls = frame->lines[line];
        const std::uint16_t dispcnt = ls.reg(0x000);
        bg_on[line].fill(false);
        smooth_bg[line].fill(false);
        if (dispcnt & 0x80) return;  // forced blank
        const unsigned mode = dispcnt & 7;
        const auto enabled = [dispcnt](unsigned bg) { return ((dispcnt >> (8 + bg)) & 1) != 0; };
        const auto smooth = [&](unsigned bg) {
            const bool mosaic = (ls.reg(0x008 + 2 * bg) & 0x40) != 0;
            return cfg.smooth_affine && N > 1 && !mosaic;
        };
        switch (mode) {
        case 0:
            for (unsigned bg = 0; bg < 4; ++bg) {
                if (enabled(bg)) {
                    bg_on[line][bg] = true;
                    text_bg(wk, ls, bg, line);
                }
            }
            break;
        case 1:
            for (unsigned bg = 0; bg < 2; ++bg) {
                if (enabled(bg)) {
                    bg_on[line][bg] = true;
                    text_bg(wk, ls, bg, line);
                }
            }
            if (enabled(2)) {
                bg_on[line][2] = true;
                smooth_bg[line][2] = smooth(2);
                affine_bg(wk, ls, 2, line);
            }
            break;
        case 2:
            for (unsigned bg = 2; bg < 4; ++bg) {
                if (enabled(bg)) {
                    bg_on[line][bg] = true;
                    smooth_bg[line][bg] = smooth(bg);
                    affine_bg(wk, ls, bg, line);
                }
            }
            break;
        case 3: case 4: case 5:
            if (enabled(2)) {
                bg_on[line][2] = true;
                smooth_bg[line][2] = smooth(2);
                bitmap_bg(wk, ls, mode, line);
            }
            break;
        default: break;
        }
        if (dispcnt & 0x1000) sprites(wk, ls, line);
    }

    // ---- per-layer scaling ----

    void build_scaled() {
        use_scaled.fill(false);
        const bool filter = cfg.layer_upscaler || factor(cfg.layer_filter) > 1;
        if (!filter || N == 1) return;
        Image in;
        for (unsigned layer = 0; layer < 5; ++layer) {
            in.resize(W, kH);
            bool any = false;
            for (unsigned line = 0; line < static_cast<unsigned>(kH); ++line) {
                for (int xi = 0; xi < W; ++xi) {
                    const std::size_t p = at(xi, line);
                    std::uint16_t c = color[layer][p];
                    if (layer < 4 && (smooth_bg[line][layer] || (lookups && tex[layer][p].texture))) c = kClear;
                    in.pixels[p] = expand(c);
                    any |= (c & kClear) == 0;
                }
            }
            if (!any) continue;
            Image up;
            if (cfg.layer_upscaler) {
                cfg.layer_upscaler(in, up);
            } else {
                scale_layer(cfg.layer_filter, in, up);
            }
            fit_layer(up, W * N, kH * N, scaled[layer]);
            use_scaled[layer] = true;
        }
    }

    // ---- pass 2: N sub-rows of one line ----

    struct Rows {
        int stride = 0;
        std::array<std::vector<std::uint32_t>, 5> layer;
        std::vector<ObjAttr> attr;
        std::vector<std::uint8_t> window;
        void reset(int n, int stride_) {
            stride = stride_;
            const auto size = static_cast<std::size_t>(n) * static_cast<std::size_t>(stride_);
            for (auto& l : layer) l.assign(size, kClearRgba);
            attr.assign(size, ObjAttr{});
            window.assign(size, 0);
        }
    };

    std::uint32_t texture_sample(const TexRef& r, int i, int j) const {
        if (r.flags & kMosaic) i = j = 0;
        if (r.flags & kHFlip) i = N - 1 - i;
        if (r.flags & kVFlip) j = N - 1 - j;
        return sample(*r.texture, r.tx * N + i, r.ty * N + j);
    }

    void layer_rows(unsigned layer, unsigned line, Rows& rows) {
        std::uint32_t* dst = rows.layer[layer].data();
        const std::uint16_t* src = color[layer].data() + at(0, line);
        const TexRef* refs = (lookups && layer < 4) ? tex[layer].data() + at(0, line) : nullptr;
        const Image* up = use_scaled[layer] ? &scaled[layer] : nullptr;
        for (int j = 0; j < N; ++j) {
            std::uint32_t* row = dst + static_cast<std::size_t>(j) * rows.stride;
            const std::uint32_t* up_row = up ? up->row(static_cast<int>(line) * N + j) : nullptr;
            for (int xi = 0; xi < W; ++xi) {
                if (refs && refs[xi].texture) {
                    for (int i = 0; i < N; ++i) {
                        const std::uint32_t c = texture_sample(refs[xi], i, j);
                        row[xi * N + i] = opaque(c) ? (c | 0xFF000000u) : kClearRgba;
                    }
                } else if (up_row) {
                    for (int i = 0; i < N; ++i) row[xi * N + i] = up_row[xi * N + i];
                } else {
                    const std::uint32_t c = expand(src[xi]);
                    for (int i = 0; i < N; ++i) row[xi * N + i] = c;
                }
            }
        }
    }

    void affine_rows(Worker& wk, unsigned bg, unsigned line, Rows& rows) {
        const LineState& ls = frame->lines[line];
        const unsigned i = bg - 2;
        const std::uint16_t cnt = ls.reg(0x008 + 2 * bg);
        const std::uint32_t params = 0x020 + 0x10 * i;
        const std::int64_t pa = ls.sreg(params), pb = ls.sreg(params + 2), pc = ls.sreg(params + 4), pd = ls.sreg(params + 6);
        const unsigned size = 128u << ((cnt >> 14) & 3);
        const bool wrap = (cnt & 0x2000) != 0;
        const unsigned char_base = ((cnt >> 2) & 3u) * 0x4000;
        const unsigned screen_base = ((cnt >> 8) & 0x1Fu) * 0x800;
        const std::uint8_t* vram = frame->vram_for(ls);
        const Extend mode = frame->scene.layers[bg];
        const std::int64_t x0 = ls.ref[2 * i], y0 = ls.ref[2 * i + 1];
        const auto mask = static_cast<std::int32_t>(size - 1);
        for (int j = 0; j < N; ++j) {
            std::uint32_t* row = rows.layer[bg].data() + static_cast<std::size_t>(j) * rows.stride;
            for (int xi = 0; xi < W; ++xi) {
                int xs;
                if (!source_column(mode, xi - E, xs)) continue;
                const bool edge = xs != xi - E;  // clamped or mirrored: no sub-pixel steps
                for (int s = 0; s < N; ++s) {
                    const int si = edge ? 0 : s, sj = edge ? 0 : j;
                    const std::int64_t fx = x0 + pa * xs + floor_div64(pa * si + pb * sj, N);
                    const std::int64_t fy = y0 + pc * xs + floor_div64(pc * si + pd * sj, N);
                    auto tx = static_cast<std::int32_t>(fx >> 8);
                    auto ty = static_cast<std::int32_t>(fy >> 8);
                    if (wrap) {
                        tx &= mask;
                        ty &= mask;
                    } else if (tx < 0 || ty < 0 || tx > mask || ty > mask) {
                        continue;
                    }
                    const unsigned map = screen_base + static_cast<unsigned>((ty >> 3) * static_cast<std::int32_t>(size >> 3) + (tx >> 3));
                    if (map >= 0x10000) continue;
                    const unsigned addr = char_base + vram[map] * 64u;
                    if (tile_art) {
                        const TileInfo info = tile_info(wk, ls, addr, true, 0, bg);
                        if (info.texture) {
                            const int u = (tx & 7) * N + static_cast<int>(((fx & 255) * N) >> 8);
                            const int v = (ty & 7) * N + static_cast<int>(((fy & 255) * N) >> 8);
                            const std::uint32_t c = sample(*info.texture, u, v);
                            if (opaque(c)) row[xi * N + s] = c | 0xFF000000u;
                            continue;
                        }
                    }
                    const unsigned a = addr + static_cast<unsigned>((ty & 7) * 8 + (tx & 7));
                    if (a < 0x10000 && vram[a]) row[xi * N + s] = expand(ls.bg_color(vram[a]));
                }
            }
        }
    }

    void bitmap_rows(Worker& wk, unsigned line, Rows& rows) {
        const LineState& ls = frame->lines[line];
        const unsigned mode = ls.reg(0x000) & 7;
        const std::int64_t pa = ls.sreg(0x020), pb = ls.sreg(0x022), pc = ls.sreg(0x024), pd = ls.sreg(0x026);
        const std::int32_t bw = mode == 5 ? 160 : 240, bh = mode == 5 ? 128 : 160;
        const unsigned page = (mode != 3 && (ls.reg(0x000) & 0x10)) ? 0xA000 : 0;
        const std::uint8_t* vram = frame->vram_for(ls);
        const Extend ext_mode = frame->scene.layers[2];
        const std::int64_t x0 = ls.ref[0], y0 = ls.ref[1];
        const Texture* t = bitmap_art ? bitmap_texture(wk, ls, mode, page) : nullptr;
        for (int j = 0; j < N; ++j) {
            std::uint32_t* row = rows.layer[2].data() + static_cast<std::size_t>(j) * rows.stride;
            for (int xi = 0; xi < W; ++xi) {
                int xs;
                if (!source_column(ext_mode, xi - E, xs)) continue;
                const bool edge = xs != xi - E;
                for (int s = 0; s < N; ++s) {
                    const int si = edge ? 0 : s, sj = edge ? 0 : j;
                    const std::int64_t fx = x0 + pa * xs + floor_div64(pa * si + pb * sj, N);
                    const std::int64_t fy = y0 + pc * xs + floor_div64(pc * si + pd * sj, N);
                    const auto tx = static_cast<std::int32_t>(fx >> 8);
                    const auto ty = static_cast<std::int32_t>(fy >> 8);
                    if (tx < 0 || ty < 0 || tx >= bw || ty >= bh) continue;
                    if (t) {
                        const std::uint32_t c = sample(*t, tx * N + static_cast<int>(((fx & 255) * N) >> 8),
                                                       ty * N + static_cast<int>(((fy & 255) * N) >> 8));
                        if (opaque(c)) row[xi * N + s] = c | 0xFF000000u;
                        continue;
                    }
                    const auto pixel = static_cast<unsigned>(ty * bw + tx);
                    if (mode == 4) {
                        const std::uint8_t index = vram[page + pixel];
                        if (index) row[xi * N + s] = expand(ls.bg_color(index));
                    } else {
                        const unsigned a = page + pixel * 2;
                        row[xi * N + s] = expand(static_cast<std::uint16_t>((vram[a] | (vram[a + 1] << 8)) & 0x7FFF));
                    }
                }
            }
        }
    }

    void obj_rows(unsigned line, Rows& rows) {
        const std::uint16_t* src = color[kObj].data() + at(0, line);
        const ObjAttr* attrs = obj_attr.data() + at(0, line);
        const std::uint8_t* win = obj_window.data() + at(0, line);
        const Image* up = use_scaled[kObj] ? &scaled[kObj] : nullptr;
        for (int j = 0; j < N; ++j) {
            const std::size_t base = static_cast<std::size_t>(j) * rows.stride;
            const std::uint32_t* up_row = up ? up->row(static_cast<int>(line) * N + j) : nullptr;
            for (int xi = 0; xi < W; ++xi) {
                for (int i = 0; i < N; ++i) {
                    const std::size_t p = base + static_cast<std::size_t>(xi * N + i);
                    rows.window[p] = win[xi];
                    if (up_row) {
                        const std::uint32_t c = up_row[xi * N + i];
                        if (!opaque(c)) continue;
                        const ObjAttr* a = nearest_attr(xi, line, i, j);
                        if (!a) continue;
                        rows.layer[kObj][p] = c | 0xFF000000u;
                        rows.attr[p] = *a;
                    } else if (attrs[xi].prio < 4) {
                        rows.layer[kObj][p] = expand(src[xi]);
                        rows.attr[p] = attrs[xi];
                    }
                }
            }
        }
    }

    // The sprite attributes for a scaled OBJ sub-pixel: its own pixel's, else the nearest drawn
    // neighbour's (scalers round silhouettes out by part of a pixel).
    const ObjAttr* nearest_attr(int xi, unsigned line, int i, int j) const {
        const ObjAttr* own = &obj_attr[at(xi, line)];
        if (own->prio < 4) return own;
        const int dx = 2 * i + 1 < N ? -1 : 1;
        const int dy = 2 * j + 1 < N ? -1 : 1;
        const int cand[3][2] = {{dx, 0}, {0, dy}, {dx, dy}};
        for (const auto& c : cand) {
            const int x = xi + c[0];
            const int y = static_cast<int>(line) + c[1];
            if (x < 0 || x >= W || y < 0 || y >= kH) continue;
            const ObjAttr* a = &obj_attr[at(x, static_cast<unsigned>(y))];
            if (a->prio < 4) return a;
        }
        return nullptr;
    }

    void special_sprite_rows(const SpecialSprite& sp, unsigned line, Rows& rows) {
        const LineState& ls = frame->lines[line];
        const unsigned n = sp.n;
        const SpriteInfo& info = *sp.info;
        const std::uint16_t a0 = ls.oam16(n * 8), a1 = ls.oam16(n * 8 + 2), a2 = ls.oam16(n * 8 + 4);
        const bool affine = (a0 & 0x100) != 0;
        const unsigned obj_mode = (a0 >> 10) & 3;
        const int w = info.art.width, h = info.art.height;
        const bool doubled = affine && (a0 & 0x200);
        const int bw = doubled ? 2 * w : w;
        const int bh = doubled ? 2 * h : h;
        const int top = a0 & 0xFF;
        const int dy = static_cast<int>((line - static_cast<unsigned>(top)) & 0xFF);
        const int xfield = a1 & 0x1FF;
        const auto priority = static_cast<std::uint8_t>((a2 >> 10) & 3);
        const bool mosaic = (a0 & 0x1000) != 0;
        const unsigned mos_h = ((ls.reg(0x04C) >> 8) & 0xFu) + 1;
        const unsigned mos_v = ((ls.reg(0x04C) >> 12) & 0xFu) + 1;
        int sample_dy = dy;
        if (mosaic) {
            sample_dy = static_cast<int>(((line - line % mos_v) - static_cast<unsigned>(top)) & 0xFF);
            if (sample_dy >= bh) sample_dy = 0;
        }
        std::int64_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            const unsigned group = ((a1 >> 9) & 0x1Fu) * 32;
            pa = static_cast<std::int16_t>(ls.oam16(group + 6));
            pb = static_cast<std::int16_t>(ls.oam16(group + 14));
            pc = static_cast<std::int16_t>(ls.oam16(group + 22));
            pd = static_cast<std::int16_t>(ls.oam16(group + 30));
        }
        const int tiles_x = w / 8;

        // Colour of the sprite's art at hi-res position (u, v) of w*N x h*N, or transparent.
        const auto art_at = [&](int u, int v) -> std::uint32_t {
            if (info.obj_texture) {
                const std::uint32_t c = sample(*info.obj_texture, u, v);
                return opaque(c) ? (c | 0xFF000000u) : kClearRgba;
            }
            const int tx = u / N, ty = v / N;
            if (!info.tile_textures.empty()) {
                const Texture* t = info.tile_textures[static_cast<std::size_t>((ty / 8) * tiles_x + tx / 8)];
                if (t) {
                    const std::uint32_t c = sample(*t, u - (tx / 8) * 8 * N, v - (ty / 8) * 8 * N);
                    return opaque(c) ? (c | 0xFF000000u) : kClearRgba;
                }
            }
            return expand(info.art.colors[static_cast<std::size_t>(ty) * w + static_cast<std::size_t>(tx)]);
        };

        for (int sx = 0; sx < bw; ++sx) {
            int screen_x;
            if (!sprite_column(xfield + sx, screen_x)) continue;
            const int x = screen_x - sx;
            int local = sx;
            if (mosaic) {
                local = (screen_x - floor_mod(screen_x, static_cast<int>(mos_h))) - x;
                if (local < 0) local = 0;
            }
            const int xi = screen_x + E;
            for (int j = 0; j < N; ++j) {
                for (int i = 0; i < N; ++i) {
                    const int si = mosaic ? 0 : i, sj = mosaic ? 0 : j;
                    int u, v;
                    if (affine) {
                        const std::int64_t lx = static_cast<std::int64_t>(local - bw / 2) * 256 + (si * 256) / N;
                        const std::int64_t ly = static_cast<std::int64_t>(sample_dy - bh / 2) * 256 + (sj * 256) / N;
                        const std::int64_t tx = ((pa * lx + pb * ly) >> 8) + (w / 2) * 256;
                        const std::int64_t ty = ((pc * lx + pd * ly) >> 8) + (h / 2) * 256;
                        if (tx < 0 || ty < 0 || tx >= w * 256 || ty >= h * 256) continue;
                        u = static_cast<int>(tx >> 8) * N + static_cast<int>(((tx & 255) * N) >> 8);
                        v = static_cast<int>(ty >> 8) * N + static_cast<int>(((ty & 255) * N) >> 8);
                    } else {
                        const int us = local * N + si, vs = sample_dy * N + sj;
                        u = (a1 & 0x1000) ? w * N - 1 - us : us;
                        v = (a1 & 0x2000) ? h * N - 1 - vs : vs;
                    }
                    const std::uint32_t c = art_at(u, v);
                    if (!opaque(c)) continue;
                    const std::size_t p = static_cast<std::size_t>(j) * rows.stride + static_cast<std::size_t>(xi * N + i);
                    if (obj_mode == 2) {
                        rows.window[p] = 1;
                        continue;
                    }
                    ObjAttr& cur = rows.attr[p];
                    if (priority < cur.prio || (priority == cur.prio && n < cur.index)) {
                        cur = {priority, static_cast<std::uint8_t>(n), obj_mode == 1};
                        rows.layer[kObj][p] = c;
                    }
                }
            }
        }
    }

    void compose_rows(unsigned line, const Rows& rows, Image& out) {
        const LineState& ls = frame->lines[line];
        const std::uint16_t dispcnt = ls.reg(0x000);
        const int row_w = W * N;
        if (dispcnt & 0x80) {
            for (int j = 0; j < N; ++j) std::fill_n(out.row(static_cast<int>(line) * N + j), row_w, kWhite);
            return;
        }
        const bool win0 = (dispcnt & 0x2000) != 0;
        const bool win1 = (dispcnt & 0x4000) != 0;
        const bool objwin = (dispcnt & 0x8000) != 0;
        const bool any_window = win0 || win1 || objwin;
        const std::uint16_t winin = ls.reg(0x048);
        const std::uint16_t winout = ls.reg(0x04A);
        const auto vspan = [](std::uint16_t v) {
            const int lo = v >> 8;
            int hi = v & 0xFF;
            if (hi > kH || lo > hi) hi = kH;
            return Span{lo, hi};
        };
        const bool in_win0_v = win0 && vspan(ls.reg(0x044)).contains(static_cast<int>(line));
        const bool in_win1_v = win1 && vspan(ls.reg(0x046)).contains(static_cast<int>(line));
        const Span win0_h = window_span(ls.reg(0x040), kW, E);
        const Span win1_h = window_span(ls.reg(0x042), kW, E);
        const std::uint16_t bldcnt = ls.reg(0x050);
        const unsigned effect = (bldcnt >> 6) & 3u;
        const unsigned eva = std::min(ls.reg(0x052) & 0x1Fu, 16u);
        const unsigned evb = std::min((ls.reg(0x052) >> 8) & 0x1Fu, 16u);
        const unsigned evy = std::min(ls.reg(0x054) & 0x1Fu, 16u);
        unsigned bg_priority[4];
        for (unsigned bg = 0; bg < 4; ++bg) bg_priority[bg] = ls.reg(0x008 + 2 * bg) & 3u;
        const std::uint32_t backdrop = expand(ls.bg_color(0));
        const bool pillarbox = frame->scene.pillarbox && E > 0;

        for (int j = 0; j < N; ++j) {
            std::uint32_t* dst = out.row(static_cast<int>(line) * N + j);
            const std::size_t base = static_cast<std::size_t>(j) * rows.stride;
            for (int xi = 0; xi < W; ++xi) {
                const int x = xi - E;
                if (pillarbox && (x < 0 || x >= kW)) {
                    std::fill_n(dst + xi * N, N, kBlack);
                    continue;
                }
                const bool in0 = in_win0_v && win0_h.contains(x);
                const bool in1 = !in0 && in_win1_v && win1_h.contains(x);
                for (int i = 0; i < N; ++i) {
                    const std::size_t p = base + static_cast<std::size_t>(xi * N + i);
                    unsigned enable = 0x3F;
                    if (any_window) {
                        if (in0) {
                            enable = winin & 0x3Fu;
                        } else if (in1) {
                            enable = (winin >> 8) & 0x3Fu;
                        } else if (objwin && rows.window[p]) {
                            enable = (winout >> 8) & 0x3Fu;
                        } else {
                            enable = winout & 0x3Fu;
                        }
                    }
                    unsigned layer[2] = {kBackdrop, kBackdrop};
                    std::uint32_t color2[2] = {backdrop, backdrop};
                    unsigned found = 0;
                    const ObjAttr& obj = rows.attr[p];
                    const std::uint32_t obj_c = rows.layer[kObj][p];
                    const bool obj_visible = (enable & 0x10) && obj.prio < 4 && opaque(obj_c);
                    for (unsigned prio = 0; prio < 4 && found < 2; ++prio) {
                        if (obj_visible && obj.prio == prio) {
                            layer[found] = kObj;
                            color2[found] = obj_c;
                            ++found;
                        }
                        for (unsigned bg = 0; bg < 4 && found < 2; ++bg) {
                            if (bg_priority[bg] != prio || !(enable & (1u << bg))) continue;
                            const std::uint32_t c = rows.layer[bg][p];
                            if (!opaque(c)) continue;
                            layer[found] = bg;
                            color2[found] = c;
                            ++found;
                        }
                    }
                    std::uint32_t result = color2[0];
                    if (enable & 0x20) {
                        const bool second_target = ((bldcnt >> (8 + layer[1])) & 1u) != 0;
                        if (layer[0] == kObj && obj.semi && second_target) {
                            result = blend8(color2[0], color2[1], eva, evb);
                        } else if ((bldcnt >> layer[0]) & 1u) {
                            switch (effect) {
                            case 1:
                                if (second_target) result = blend8(color2[0], color2[1], eva, evb);
                                break;
                            case 2: result = brighten8(color2[0], evy); break;
                            case 3: result = darken8(color2[0], evy); break;
                            default: break;
                            }
                        }
                    }
                    dst[xi * N + i] = result | 0xFF000000u;
                }
            }
            if (ls.reg(0x002) & 1) {
                // Green swap: each pixel pair exchanges its green components, sub-pixel by sub-pixel.
                for (int xi = 0; xi + 1 < W; xi += 2) {
                    for (int i = 0; i < N; ++i) {
                        std::uint32_t& a = dst[xi * N + i];
                        std::uint32_t& b = dst[(xi + 1) * N + i];
                        const std::uint32_t ga = a & 0x0000FF00u, gb = b & 0x0000FF00u;
                        a = (a & ~0x0000FF00u) | gb;
                        b = (b & ~0x0000FF00u) | ga;
                    }
                }
            }
        }
    }

    void pass2_line(Worker& wk, unsigned line, Rows& rows, Image& out) {
        rows.reset(N, W * N);
        for (unsigned bg = 0; bg < 4; ++bg) {
            if (!bg_on[line][bg]) continue;
            if (smooth_bg[line][bg]) {
                const unsigned mode = frame->lines[line].reg(0x000) & 7;
                if (mode >= 3) {
                    bitmap_rows(wk, line, rows);
                } else {
                    affine_rows(wk, bg, line, rows);
                }
            } else {
                layer_rows(bg, line, rows);
            }
        }
        obj_rows(line, rows);
        for (const SpecialSprite& sp : special[line]) special_sprite_rows(sp, line, rows);
        compose_rows(line, rows, out);
    }

    // The 24x24 neighbourhood of each newly seen BG tile on its layer (for AI upscaling with
    // context); the tile sits at (8, 8).
    void send_contexts() {
        if (!cfg.sink) return;
        for (const Pending& p : pending) {
            Art ctx;
            ctx.width = ctx.height = 24;
            ctx.colors.assign(24 * 24, kClear);
            for (int y = 0; y < 24; ++y) {
                for (int x = 0; x < 24; ++x) {
                    const int sx = p.x - 8 + x, sy = p.y - 8 + y;
                    if (sx < 0 || sx >= W || sy < 0 || sy >= kH) continue;
                    ctx.colors[static_cast<std::size_t>(y * 24 + x)] = color[p.layer][at(sx, static_cast<unsigned>(sy))];
                }
            }
            cfg.sink->context(KeyType::Tile, p.key, ctx, 8, 8);
        }
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

void Renderer::render(const Frame& frame, const RenderConfig& config, Image& out) {
    Impl& m = *impl_;
    m.frame = &frame;
    m.cfg = config;
    m.N = std::clamp(config.scale, 1, 16);
    m.E = std::max(0, config.extra);
    m.W = kW + 2 * m.E;
    m.tile_art = config.textures && (config.textures->has(KeyType::Tile) || config.textures->has(KeyType::TileIndices));
    m.obj_art = config.textures && (config.textures->has(KeyType::Obj) || config.textures->has(KeyType::ObjIndices));
    m.bitmap_art = config.textures && config.textures->has(KeyType::Bitmap);
    m.lookups = m.tile_art || m.obj_art || m.bitmap_art || config.sink;

    const auto pixels = static_cast<std::size_t>(m.W) * kH;
    for (auto& c : m.color) c.assign(pixels, kClear);
    for (auto& t : m.tex) {
        if (m.lookups) {
            t.assign(pixels, TexRef{});
        } else {
            t.clear();
        }
    }
    m.obj_attr.assign(pixels, ObjAttr{});
    m.obj_window.assign(pixels, 0);
    for (auto& s : m.special) s.clear();
    m.workers.clear();
    m.pending.clear();

    parallel_for(kH, [&](int begin, int end) {
        Worker& wk = m.new_worker();
        for (int line = begin; line < end; ++line) m.pass1_line(wk, static_cast<unsigned>(line));
    });
    m.send_contexts();
    m.build_scaled();

    out.resize(m.W * m.N, kH * m.N);
    parallel_for(kH, [&](int begin, int end) {
        Worker& wk = m.new_worker();
        Impl::Rows rows;
        for (int line = begin; line < end; ++line) m.pass2_line(wk, static_cast<unsigned>(line), rows, out);
    });
}

}  // namespace anygba::video::hd
