#include "anygba/system.hpp"
#include "support/corpus.h"
#include "support/temp_dir.h"
#include "video/hd/dump.h"
#include "video/hd/frame.h"
#include "video/hd/json.h"
#include "video/hd/keys.h"
#include "video/hd/layer_scale.h"
#include "video/hd/pack.h"
#include "video/hd/png_io.h"
#include "video/hd/renderer.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hd = anygba::video::hd;
using anygba::System;
using anygba::video::Image;

namespace {

void put16(std::vector<std::uint8_t>& mem, std::size_t off, std::uint16_t v) {
    mem[off] = static_cast<std::uint8_t>(v);
    mem[off + 1] = static_cast<std::uint8_t>(v >> 8);
}

unsigned tile_index(unsigned p) { return 1 + ((p % 8) + (p / 8)) % 15; }

// BG0 shows tile 1 (4bpp) four times in its top row: plain, h-flipped, v-flipped and with
// palette bank 1. One 16x16 sprite (OBJ tiles 0-3) sits at (0, 64).
std::unique_ptr<hd::Frame> synthetic_frame() {
    anygba::Memory mem;
    put16(mem.io, 0x000, 0x1140);    // mode 0, BG0 and OBJ on, 1D OBJ mapping
    put16(mem.io, 0x008, 31u << 8);  // BG0: char base 0, screen base 31 (0xF800)
    for (unsigned i = 1; i < 16; ++i) {
        put16(mem.palette, i * 2, static_cast<std::uint16_t>(i * 2));                  // bank 0: reds
        put16(mem.palette, (16 + i) * 2, static_cast<std::uint16_t>((i * 2) << 5));    // bank 1: greens
        put16(mem.palette, (256 + i) * 2, static_cast<std::uint16_t>((i * 2) << 10));  // OBJ: blues
    }
    for (unsigned p = 0; p < 64; ++p) {
        mem.vram[32 + p / 2] = static_cast<std::uint8_t>(mem.vram[32 + p / 2] | (tile_index(p) << ((p & 1) * 4)));
    }
    put16(mem.vram, 0xF800, 1);
    put16(mem.vram, 0xF802, 1 | 0x400);
    put16(mem.vram, 0xF804, 1 | 0x800);
    put16(mem.vram, 0xF806, 1 | 0x1000);
    for (unsigned b = 0; b < 4 * 32; ++b) mem.vram[0x10000 + b] = static_cast<std::uint8_t>(0x11 * (1 + b % 7));
    put16(mem.oam, 0, 64);      // y 64, square
    put16(mem.oam, 2, 0x4000);  // x 0, 16x16
    put16(mem.oam, 4, 0);       // tile 0, priority 0, palette 0
    for (unsigned n = 1; n < 128; ++n) put16(mem.oam, n * 8, 0x200);  // hidden
    auto f = std::make_unique<hd::Frame>();
    for (auto& ls : f->lines) hd::capture_line(mem, {}, ls);
    f->vram[0] = mem.vram;
    f->number = 1;
    return f;
}

hd::Art tile_art(const hd::Frame& f, unsigned bank) {
    hd::Art a;
    a.width = a.height = 8;
    for (unsigned p = 0; p < 64; ++p) {
        a.indices.push_back(static_cast<std::uint8_t>(tile_index(p)));
        a.colors.push_back(f.lines[0].bg_color(bank * 16 + tile_index(p)));
    }
    return a;
}

std::uint32_t pattern(int u, int v, int seed) {
    return static_cast<std::uint32_t>((u * 7 + seed) & 255) | (static_cast<std::uint32_t>((v * 5 + seed * 3) & 255) << 8) |
           (static_cast<std::uint32_t>((seed * 40) & 255) << 16) | 0xFF000000u;
}

void write_pattern(const std::filesystem::path& file, int w, int h, int seed) {
    std::vector<std::uint32_t> px;
    for (int v = 0; v < h; ++v) {
        for (int u = 0; u < w; ++u) px.push_back(pattern(u, v, seed));
    }
    REQUIRE_FALSE(hd::save_png(file, px.data(), w, h));
}

std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Counts lookups that found art.
class Counting final : public hd::TextureSource {
public:
    explicit Counting(hd::TextureSource& inner) : inner_(inner) {}
    bool has(hd::KeyType t) const override { return inner_.has(t); }
    const hd::Texture* find(hd::KeyType t, std::uint64_t h, int w, int hh, int s) override {
        const hd::Texture* r = inner_.find(t, h, w, hh, s);
        if (r) ++hits;
        return r;
    }
    std::atomic<int> hits{0};

private:
    hd::TextureSource& inner_;
};

}  // namespace

TEST_CASE("hd pack: 4x art replaces tiles exactly, flipped, per palette and per index") {
    anygba::test::TempDir dir("anygba-hd-pack");
    const auto frame = synthetic_frame();
    const std::uint64_t key0 = hd::color_key(hd::KeyType::Tile, tile_art(*frame, 0));
    const std::uint64_t key1 = hd::color_key(hd::KeyType::Tile, tile_art(*frame, 1));
    const std::uint64_t ikey = hd::index_key(hd::KeyType::TileIndices, tile_art(*frame, 0));
    CHECK(key0 != key1);
    CHECK(ikey == hd::index_key(hd::KeyType::TileIndices, tile_art(*frame, 1)));
    write_pattern(dir.path / "tiles" / (hd::key_name(hd::KeyType::Tile, key0) + ".png"), 32, 32, 1);

    hd::Pack pack(dir.path);
    CHECK(pack.size() == 1);
    hd::Renderer renderer;
    hd::RenderConfig cfg;
    cfg.scale = 4;
    cfg.textures = &pack;
    Image out;
    renderer.render(*frame, cfg, out);
    int wrong = 0;
    for (int v = 0; v < 32; ++v) {
        for (int u = 0; u < 32; ++u) {
            wrong += out.row(v)[u] != pattern(u, v, 1);            // plain
            wrong += out.row(v)[32 + u] != pattern(31 - u, v, 1);  // h-flipped
            wrong += out.row(v)[64 + u] != pattern(u, 31 - v, 1);  // v-flipped
        }
    }
    CHECK(wrong == 0);
    // Bank 1 looks different, so art keyed by bank 0's colours does not apply: the original, 4x.
    CHECK(out.row(0)[96] == hd::expand(frame->lines[0].bg_color(16 + tile_index(0))));

    // Palette-independent art serves bank 1; bank 0 keeps its own art.
    write_pattern(dir.path / (hd::key_name(hd::KeyType::TileIndices, ikey) + ".png"), 16, 16, 2);
    CHECK(pack.reload());
    renderer.render(*frame, cfg, out);
    wrong = 0;
    for (int v = 0; v < 32; ++v) {
        for (int u = 0; u < 32; ++u) {
            wrong += out.row(v)[u] != pattern(u, v, 1);
            wrong += out.row(v)[96 + u] != pattern(u / 2, v / 2, 2);  // 2x art shown at 4x: nearest
        }
    }
    CHECK(wrong == 0);
}

TEST_CASE("hd dump: whole sprites, tiles and index; sprite art; wrong sizes; hot reload") {
    anygba::test::TempDir dump_dir("anygba-hd-dump");
    anygba::test::TempDir pack_dir("anygba-hd-objpack");
    const auto frame = synthetic_frame();
    hd::Renderer renderer;
    Image out;
    {
        hd::Dumper dumper(dump_dir.path, "TEST", "synthetic");
        hd::RenderConfig cfg;
        cfg.sink = &dumper;
        renderer.render(*frame, cfg, out);
        renderer.render(*frame, cfg, out);
        CHECK_FALSE(dumper.wait());
        CHECK(dumper.entries() == 3);  // tile 1 in two palettes and the sprite
    }
    const auto index = hd::json::parse(read_text(dump_dir.path / "index.json"));
    CHECK(index.str("game") == "TEST");
    const auto* entries = index.get("entries");
    REQUIRE(entries);
    REQUIRE(entries->array.size() == 3);
    std::string obj_key;
    for (const auto& e : entries->array) {
        CHECK(std::filesystem::exists(dump_dir.path / e.str("file")));
        CHECK(e.num("frames") == 1);  // two renders of frame 1
        if (e.str("kind") == "obj") {
            obj_key = e.str("key");
            CHECK(e.num("width") == 16);
            CHECK(e.str("indices").starts_with("oi-"));
        } else {
            CHECK(e.str("kind") == "tile");
            CHECK(e.str("layer") == "bg0");
            CHECK(std::filesystem::exists(dump_dir.path / "tiles" / (e.str("key") + ".context.png")));
        }
    }
    REQUIRE(obj_key.starts_with("o-"));
    std::string error;
    const auto dumped = hd::load_png(dump_dir.path / "objs" / (obj_key + ".png"), &error);
    REQUIRE(dumped);
    CHECK(dumped->width == 16);

    // The dump folder is an identity pack: 1x pictures stay the same.
    {
        hd::Pack identity(dump_dir.path);
        Counting counting(identity);
        hd::RenderConfig plain;
        Image reference, packed;
        renderer.render(*frame, plain, reference);
        plain.textures = &counting;
        renderer.render(*frame, plain, packed);
        CHECK(reference.pixels == packed.pixels);
        CHECK(counting.hits > 0);
    }

    // Whole-sprite art at 4x; a wrongly sized tile image is ignored with a warning.
    write_pattern(pack_dir.path / (obj_key + ".png"), 64, 64, 3);
    write_pattern(pack_dir.path / (hd::key_name(hd::KeyType::Tile, hd::color_key(hd::KeyType::Tile, tile_art(*frame, 0))) + ".png"),
                  20, 20, 4);
    hd::Pack pack(pack_dir.path);
    hd::RenderConfig cfg;
    cfg.scale = 4;
    cfg.textures = &pack;
    renderer.render(*frame, cfg, out);
    int wrong = 0;
    for (int v = 0; v < 64; ++v) {
        for (int u = 0; u < 64; ++u) wrong += out.row(256 + v)[u] != pattern(u, v, 3);
    }
    CHECK(wrong == 0);
    CHECK(out.row(0)[0] == hd::expand(frame->lines[0].bg_color(tile_index(0))));
    const auto warnings = pack.take_warnings();
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("20x20") != std::string::npos);

    // Hot reload: a file changed while watching is picked up between frames.
    pack.watch(20);
    write_pattern(pack_dir.path / (obj_key + ".png"), 32, 32, 5);
    bool applied = false;
    for (int i = 0; i < 300 && !applied; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        applied = pack.apply_changes();
    }
    REQUIRE(applied);
    renderer.render(*frame, cfg, out);
    CHECK(out.row(256)[0] == pattern(0, 0, 5));
    CHECK(out.row(256 + 2)[2] == pattern(1, 1, 5));  // 2x art at 4x
}

TEST_CASE("hd dump: identity round trip on corpus demos gives the PPU's frames") {
    struct Case {
        const char* rom;
        std::uint64_t frames;
    };
    for (const Case c : {Case{"tonc/bin/brin_demo.gba", 40}, Case{"tonc/bin/obj_aff.gba", 40},
                         Case{"tonc/bin/bm_modes.gba", 20}, Case{"tonc/bin/m7_demo.gba", 40}}) {
        auto rom = anygba::test::read_corpus_file(c.rom);
        if (!rom) {
            MESSAGE("corpus not fetched; skipping");
            return;
        }
        anygba::test::TempDir dir("anygba-hd-roundtrip");
        auto recorder = std::make_unique<hd::Recorder>();
        hd::Renderer renderer;
        Image out;
        int bad = 0, frames = 0;
        std::unique_ptr<hd::Dumper> dumper = std::make_unique<hd::Dumper>(dir.path, "TONC", c.rom);
        std::unique_ptr<hd::Pack> pack;
        std::unique_ptr<Counting> counting;
        bool ready = false;
        anygba::Options options;
        options.log_misses = false;
        System sys(std::move(*rom), nullptr, options);
        sys.on_line_rendered = [&](System& s, unsigned line) { ready = recorder->on_line(s, line); };
        sys.on_vblank = [&](System& s) {
            if (!ready) return;
            hd::RenderConfig cfg;
            if (s.frames() <= c.frames) {
                cfg.sink = dumper.get();  // dump the first part
            } else {
                if (!pack) {
                    CHECK_FALSE(dumper->wait());
                    dumper.reset();
                    pack = std::make_unique<hd::Pack>(dir.path);
                    counting = std::make_unique<Counting>(*pack);
                }
                cfg.textures = counting.get();  // replay the same frames with the dump as a pack
            }
            renderer.render(recorder->frame(), cfg, out);
            const auto& fb = s.framebuffer();
            for (std::size_t i = 0; i < fb.size(); ++i) {
                if (out.pixels[i] != hd::expand(static_cast<std::uint16_t>(fb[i] & 0x7FFF))) {
                    ++bad;
                    break;
                }
            }
            ++frames;
            if (s.frames() >= 2 * c.frames) s.stop();
        };
        sys.run();
        INFO(std::string(c.rom));
        CHECK(frames >= static_cast<int>(2 * c.frames) - 2);
        CHECK(bad == 0);
        CHECK(counting != nullptr);
        if (counting) CHECK(counting->hits > 0);
    }
}

TEST_CASE("hd layer scaling keeps transparency and edge colours") {
    // A 4x4 red square with a transparent border, scaled with xBRZ 3x.
    Image in;
    in.resize(8, 8);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) in.row(y)[x] = (x >= 2 && x < 6 && y >= 2 && y < 6) ? 0xFF0000FFu : 0u;
    }
    Image out;
    hd::scale_layer(anygba::video::Scaler::Xbrz3x, in, out);
    REQUIRE(out.width == 24);
    int opaque = 0;
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 24; ++x) {
            const std::uint32_t c = out.row(y)[x];
            if ((c >> 24) >= 128) {
                ++opaque;
                CHECK((c & 0xFFFFFFu) == 0x0000FFu);  // no dark fringe from the transparent area
            }
        }
    }
    CHECK(opaque > 100);   // about 12x12 with rounded corners
    CHECK(opaque <= 144);
    CHECK((out.row(0)[0] >> 24) == 0);
}

TEST_CASE("hd keys and json") {
    CHECK(hd::key_name(hd::KeyType::ObjIndices, 0x0123456789abcdefull) == "oi-0123456789abcdef");
    const auto k = hd::parse_key("ti-00000000000000ff");
    REQUIRE(k);
    CHECK(k->type == hd::KeyType::TileIndices);
    CHECK(k->hash == 255);
    CHECK_FALSE(hd::parse_key("t-123"));
    CHECK_FALSE(hd::parse_key("x-0123456789abcdef"));
    CHECK_FALSE(hd::parse_key("t-0123456789abcdef.context"));
    // FNV-1a 64 of the documented byte sequence: 'T', width and height as 16-bit little-endian,
    // then each pixel as 16-bit little-endian (0x8000 for transparent).
    hd::Art a;
    a.width = a.height = 1;
    a.colors = {0x7FFF};
    hd::Fnv f;
    for (const std::uint8_t b : {std::uint8_t('T'), std::uint8_t(1), std::uint8_t(0), std::uint8_t(1), std::uint8_t(0),
                                 std::uint8_t(0xFF), std::uint8_t(0x7F)}) {
        f.byte(b);
    }
    CHECK(hd::color_key(hd::KeyType::Tile, a) == f.value());

    const auto v = hd::json::parse(R"({"a": [1, 2.5, "x\"yé"], "b": {"c": true, "d": null}})");
    CHECK(v.get("a")->array.size() == 3);
    CHECK(v.get("a")->array[1].number == 2.5);
    CHECK(v.get("a")->array[2].string == "x\"y\xc3\xa9");
    CHECK(v.get("b")->get("c")->boolean);
    CHECK_THROWS(hd::json::parse("{\"a\": }"));
    CHECK(hd::json::quote("a\"b\n") == "\"a\\\"b\\n\"");

    hd::DumpFilter filter;
    CHECK_FALSE(hd::parse_dump_filter("objs,bg2", filter));
    CHECK_FALSE(filter.tiles);
    CHECK(filter.objs);
    CHECK(filter.bg[2]);
    CHECK_FALSE(filter.bg[0]);
    CHECK(hd::parse_dump_filter("sprites", filter));
}
