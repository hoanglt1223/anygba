#include "video/hd/widescreen.h"

#include <algorithm>

// Widescreen provider for the overworld of the Pokemon Ruby/Sapphire/Emerald engine (the layout
// documented by the pret/pokeemerald decompilation). The game keeps a 32x32-tile ring buffer per
// map layer, one metatile (16x16) wider than the screen, and redraws the slice that scrolls in.
// This provider rebuilds the same tile map entries for any position from the map grid and the
// tilesets' metatile definitions, so the extra columns show the real map.
//
// Settings (widescreen/<code>.toml): map_header (gMapHeader), backup_layout (gBackupMapLayout),
// save_block1_ptr (gSaveBlock1Ptr). Before extending, it predicts the visible part of BG1-BG3 and
// requires it to match what the game drew; otherwise the frame is pillarboxed.
namespace anygba::video::hd {

namespace {

constexpr std::uint32_t kPrimaryMetatiles = 512;
constexpr std::uint32_t kTotalMetatiles = 1024;
constexpr std::uint16_t kBlankTile = 0x3014;  // what the bottom layer gets under normal metatiles
constexpr std::uint32_t kUndefined = 0x3FF;   // grid cells outside the map and its connections
// Share of visible tiles (those drawn on BG1 or BG2) that must match the game's own tile maps.
constexpr double kRequiredMatch = 0.97;

bool in_rom(std::uint32_t a) { return a >= 0x08000000 && a < 0x0A000000; }
bool in_ram(std::uint32_t a) { return (a >> 24) == 0x02 || (a >> 24) == 0x03; }

class PokeemeraldField final : public MapProvider {
public:
    PokeemeraldField(std::uint32_t map_header, std::uint32_t backup_layout, std::uint32_t save_block1_ptr)
        : map_header_(map_header), backup_layout_(backup_layout), save_block1_ptr_(save_block1_ptr) {}

    bool extend(const Memory& mem, Frame& frame, int extra) override {
        if (!read_state(mem)) return false;
        const LineState& ls = frame.lines[kScreenHeight / 2];
        // BG1-BG3 show the top, middle and bottom map layers with the same scroll.
        const int hofs = ls.reg(0x014) & 0x1FF;
        const int vofs = ls.reg(0x016) & 0x1FF;
        for (unsigned bg = 1; bg < 4; ++bg) {
            const std::uint16_t cnt = ls.reg(0x008 + 2 * bg);
            if ((cnt & 0xC080) != 0) return false;  // 256x256, 4bpp, as the engine sets them
            if ((ls.reg(0x010 + 4 * bg) & 0x1FF) != hofs || (ls.reg(0x012 + 4 * bg) & 0x1FF) != vofs) return false;
            screen_base_[bg] = ((cnt >> 8) & 0x1Fu) * 0x800;
        }
        const std::uint8_t* vram = frame.vram_for(ls);

        // Which unwrapped tile column/row (screen + scroll, / 8) the camera's metatile (gSaveBlock1
        // pos) starts at. The last frame's answer usually still holds; otherwise search around
        // the scroll position on a sample of the screen, then check the best candidate fully.
        const int t0 = hofs / 8, r0 = vofs / 8;
        const auto good = [&](int kx, int ky) {
            const Score s = score(vram, kx, ky, t0, r0, hofs, vofs, 1);
            return s.nontrivial >= 30 && s.matched >= kRequiredMatch * s.nontrivial;
        };
        bool found = false;
        int best_kx = 0, best_ky = 0;
        if (have_alignment_) {
            for (int dy = -2; dy <= 2 && !found; ++dy) {
                for (int dx = -2; dx <= 2 && !found; ++dx) {
                    if (good(last_kx_ + dx, last_ky_ + dy)) {
                        found = true;
                        best_kx = last_kx_ + dx;
                        best_ky = last_ky_ + dy;
                    }
                }
            }
        }
        if (!found) {
            int best = -1;
            for (int ky = r0 - 12; ky <= r0 + 12; ++ky) {
                for (int kx = t0 - 12; kx <= t0 + 12; ++kx) {
                    const int s = score(vram, kx, ky, t0, r0, hofs, vofs, 3).matched;
                    if (s > best) {
                        best = s;
                        best_kx = kx;
                        best_ky = ky;
                    }
                }
            }
            found = good(best_kx, best_ky);
        }
        have_alignment_ = found;
        if (!found) return false;
        last_kx_ = best_kx;
        last_ky_ = best_ky;

        // Every tile the widened picture can show, with a margin for scroll changes between lines.
        const int x0 = (hofs - extra - 15) / 8 - 1;  // hofs - extra may be negative: round down
        const int width = (kScreenWidth + 2 * extra) / 8 + 6;
        const int y0 = vofs / 8 - 2;
        const int height = kScreenHeight / 8 + 5;
        for (unsigned bg = 1; bg < 4; ++bg) {
            MapExtension& ext = frame.extension[bg];
            ext.x0 = x0;
            ext.y0 = y0;
            ext.width = width;
            ext.height = height;
            ext.entries.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), -1);
        }
        for (int r = 0; r < height; ++r) {
            for (int c = 0; c < width; ++c) {
                const auto e = predict(best_kx, best_ky, x0 + c, y0 + r);
                for (unsigned bg = 1; bg < 4; ++bg) {
                    frame.extension[bg].entries[static_cast<std::size_t>(r) * static_cast<std::size_t>(width) +
                                                static_cast<std::size_t>(c)] = e[bg];
                }
            }
        }
        return true;
    }

private:
    struct Score {
        int matched = 0;     // visible tiles whose three layers all match
        int nontrivial = 0;  // visible tiles with something drawn on any layer
    };

    // Compares the prediction for alignment (kx, ky) with the game's tile maps over the screen,
    // every `step`-th tile in both directions.
    Score score(const std::uint8_t* vram, int kx, int ky, int t0, int r0, int hofs, int vofs, int step) const {
        Score s;
        for (int r = r0; r <= (vofs + kScreenHeight - 1) / 8; r += step) {
            for (int t = t0; t <= (hofs + kScreenWidth - 1) / 8; t += step) {
                const auto e = predict(kx, ky, t, r);
                bool all = true, drawn = false;
                for (unsigned bg = 1; bg < 4; ++bg) {
                    const unsigned addr = screen_base_[bg] + static_cast<unsigned>(((r & 31) * 32 + (t & 31)) * 2);
                    const unsigned actual = vram[addr] | (vram[addr + 1] << 8);
                    all = all && actual == e[bg];
                    drawn = drawn || actual != (bg == 3 ? kBlankTile : 0u);
                }
                if (!drawn) continue;
                ++s.nontrivial;
                s.matched += all;
            }
        }
        return s;
    }

    bool read_state(const Memory& mem) {
        layout_ = peek(mem, map_header_, 4);
        if (!in_rom(layout_)) return false;
        border_ = peek(mem, layout_ + 8, 4);
        const std::uint32_t primary = peek(mem, layout_ + 16, 4);
        const std::uint32_t secondary = peek(mem, layout_ + 20, 4);
        if (!in_rom(primary) || !in_rom(secondary) || !in_rom(border_)) return false;
        metatiles_[0] = peek(mem, primary + 12, 4);
        attributes_[0] = peek(mem, primary + 16, 4);
        metatiles_[1] = peek(mem, secondary + 12, 4);
        attributes_[1] = peek(mem, secondary + 16, 4);
        for (int i = 0; i < 2; ++i) {
            if (!in_rom(metatiles_[i]) || !in_rom(attributes_[i])) return false;
        }
        grid_w_ = static_cast<std::int32_t>(peek(mem, backup_layout_, 4));
        grid_h_ = static_cast<std::int32_t>(peek(mem, backup_layout_ + 4, 4));
        grid_ = peek(mem, backup_layout_ + 8, 4);
        if (grid_w_ <= 0 || grid_h_ <= 0 || grid_w_ > 0x200 || grid_h_ > 0x200 || !in_ram(grid_)) return false;
        const std::uint32_t sb1 = peek(mem, save_block1_ptr_, 4);
        if (!in_ram(sb1)) return false;
        pos_x_ = static_cast<std::int16_t>(peek(mem, sb1, 2));
        pos_y_ = static_cast<std::int16_t>(peek(mem, sb1 + 2, 2));
        mem_ = &mem;
        return true;
    }

    // Metatile id at map grid position (x, y) (grid coordinates include the margin).
    std::uint32_t metatile_at(int x, int y) const {
        if (x >= 0 && x < grid_w_ && y >= 0 && y < grid_h_) {
            const std::uint32_t id = peek(*mem_, grid_ + 2u * static_cast<std::uint32_t>(x + grid_w_ * y), 2) & 0x3FFu;
            if (id != kUndefined) return id;
        }
        const int i = ((x + 1) & 1) + ((y + 1) & 1) * 2;  // the layout's 2x2 border block
        return peek(*mem_, border_ + 2u * static_cast<std::uint32_t>(i), 2) & 0x3FFu;
    }

    // Tile map entries of BG1, BG2 and BG3 (index 1-3) at unwrapped tile (t, r), with the camera
    // metatile starting at tile (kx, ky).
    std::array<std::uint16_t, 4> predict(int kx, int ky, int t, int r) const {
        const int dx = t - kx, dy = r - ky;
        const int mx = pos_x_ + (dx >= 0 ? dx / 2 : -((-dx + 1) / 2));
        const int my = pos_y_ + (dy >= 0 ? dy / 2 : -((-dy + 1) / 2));
        const int sub = (dx & 1) + 2 * (dy & 1);
        std::uint32_t id = metatile_at(mx, my);
        if (id >= kTotalMetatiles) id = 0;
        const int set = id < kPrimaryMetatiles ? 0 : 1;
        const std::uint32_t local = set ? id - kPrimaryMetatiles : id;
        const std::uint32_t tiles = metatiles_[set] + local * 16;
        const std::uint32_t attr = peek(*mem_, attributes_[set] + local * 2, 2);
        const auto tile = [&](int i) { return static_cast<std::uint16_t>(peek(*mem_, tiles + 2u * static_cast<std::uint32_t>(i), 2)); };
        std::array<std::uint16_t, 4> e{};
        switch ((attr >> 12) & 0xF) {
        case 1:  // covered: bottom on BG3, top on BG2
            e[3] = tile(sub);
            e[2] = tile(4 + sub);
            e[1] = 0;
            break;
        case 2:  // split: bottom on BG3, top on BG1
            e[3] = tile(sub);
            e[2] = 0;
            e[1] = tile(4 + sub);
            break;
        default:  // normal: bottom on BG2, top on BG1
            e[3] = kBlankTile;
            e[2] = tile(sub);
            e[1] = tile(4 + sub);
            break;
        }
        return e;
    }

    std::uint32_t map_header_, backup_layout_, save_block1_ptr_;
    std::array<unsigned, 4> screen_base_{};
    const Memory* mem_ = nullptr;
    std::uint32_t layout_ = 0, border_ = 0, grid_ = 0;
    std::array<std::uint32_t, 2> metatiles_{}, attributes_{};
    std::int32_t grid_w_ = 0, grid_h_ = 0;
    int pos_x_ = 0, pos_y_ = 0;
    bool have_alignment_ = false;
    int last_kx_ = 0, last_ky_ = 0;
};

}  // namespace

std::unique_ptr<MapProvider> make_pokeemerald_field(const std::map<std::string, std::uint32_t>& settings,
                                                    std::string* error) {
    std::uint32_t v[3];
    const char* const names[3] = {"map_header", "backup_layout", "save_block1_ptr"};
    for (int i = 0; i < 3; ++i) {
        const auto it = settings.find(names[i]);
        if (it == settings.end()) {
            if (error) *error = std::string("pokeemerald-field needs the setting ") + names[i];
            return nullptr;
        }
        v[i] = it->second;
    }
    return std::make_unique<PokeemeraldField>(v[0], v[1], v[2]);
}

}  // namespace anygba::video::hd
