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
#include "video/hd/widescreen.h"
#include "video/hd/ai.h"
#include "video/hd/onnx_patch.h"
#include "video/settings.h"

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
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
        hd::DumpFilter every;
        every.min_frames = 1;
        hd::Dumper dumper(dump_dir.path, "TEST", "synthetic", every);
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
    CHECK_FALSE(hd::parse_dump_filter("tiles,frames=5", filter));
    CHECK(filter.min_frames == 5);
    CHECK(hd::parse_dump_filter("frames=0", filter));
}

TEST_CASE("hd dump: art is written once it has been seen in min_frames frames") {
    anygba::test::TempDir dir("anygba-hd-minframes");
    auto frame = synthetic_frame();
    hd::Renderer renderer;
    Image out;
    hd::Dumper dumper(dir.path, "TEST", "synthetic");  // default: 3 frames
    hd::RenderConfig cfg;
    cfg.sink = &dumper;
    for (std::uint64_t n = 1; n <= 2; ++n) {
        frame->number = n;
        renderer.render(*frame, cfg, out);
    }
    CHECK_FALSE(dumper.wait());
    CHECK(dumper.entries() == 0);  // a fade step lasts a frame or two: not dumped
    frame->number = 3;
    renderer.render(*frame, cfg, out);
    CHECK_FALSE(dumper.wait());
    CHECK(dumper.entries() == 3);
    const auto plain = hd::key_name(hd::KeyType::Tile, hd::color_key(hd::KeyType::Tile, tile_art(*frame, 0)));
    CHECK(std::filesystem::exists(dir.path / "tiles" / (plain + ".png")));
    CHECK(std::filesystem::exists(dir.path / "tiles" / (plain + ".context.png")));
}

// ---- widescreen ----

TEST_CASE("widescreen rules: scenes by memory, layer modes, patches and errors") {
    const auto rules_text = R"(
title = "Test"
[default]
mode = "pillarbox"

[[scene]]
name = "field"
when = ["u16 0x03000010 == 0x1234", "u8 0x02000000 & 0x80"]
hud = ["bg0", "obj"]
clamp = ["bg3"]
mirror = ["bg2"]

[[patch]]
address = 0x08000100
value = 0x46C0
)";
    auto rules = hd::Rules::parse(rules_text, "test.toml");
    CHECK(rules.title() == "Test");
    REQUIRE(rules.rom_patches().size() == 1);
    CHECK(rules.rom_patches().at(0x08000100) == 0x46C0);
    anygba::Memory mem;
    mem.iwram[0x10] = 0x34;
    mem.iwram[0x11] = 0x12;
    mem.ewram[0] = 0x80;
    auto frame = std::make_unique<hd::Frame>();
    CHECK(rules.apply(mem, *frame, 22) == "field");
    CHECK_FALSE(frame->scene.pillarbox);
    CHECK(frame->scene.layers[0] == hd::Extend::Hide);
    CHECK(frame->scene.layers[1] == hd::Extend::Map);
    CHECK(frame->scene.layers[2] == hd::Extend::Mirror);
    CHECK(frame->scene.layers[3] == hd::Extend::Clamp);
    CHECK(frame->scene.layers[4] == hd::Extend::Hide);
    mem.ewram[0] = 0x7F;
    CHECK(rules.apply(mem, *frame, 22) == "default");
    CHECK(frame->scene.pillarbox);

    const auto error_of = [](const char* text) {
        try {
            (void)hd::Rules::parse(text, "bad.toml");
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    CHECK(error_of("[[scene]]\nname = \"x\"\nwhen = [\"u16 0x03000000 ~ 1\"]\n").find("operator") != std::string::npos);
    CHECK(error_of("[[scene]]\nname = \"x\"\n").find("needs a 'when'") != std::string::npos);
    CHECK(error_of("[default]\nhud = [\"bg9\"]\n").find("unknown layer") != std::string::npos);
    CHECK(error_of("[default]\nclamp = [\"obj\"]\n").find("sprites") != std::string::npos);
    CHECK(error_of("[default]\nprovider = \"nope\"\n").find("unknown widescreen provider") != std::string::npos);
    CHECK(error_of("[default]\nprovider = \"pokeemerald-field\"\n").find("map_header") != std::string::npos);
    CHECK(error_of("[[patch]]\naddress = 0x08000101\nvalue = 1\n").find("even ROM address") != std::string::npos);
    CHECK(error_of("title = ").find("bad.toml:") == 0);
}

TEST_CASE("widescreen: map, hud, clamp and pillarbox fill the extra columns; the middle stays") {
    auto frame = synthetic_frame();
    // BG0's whole map shows tile 1, so its own map can fill the extra columns.
    for (unsigned i = 0; i < 32 * 32; ++i) {
        frame->vram[0][0xF800 + 2 * i] = 1;
        frame->vram[0][0xF800 + 2 * i + 1] = 0;
    }
    hd::Renderer renderer;
    Image normal, wide;
    renderer.render(*frame, {}, normal);
    hd::RenderConfig cfg;
    cfg.extra = 8;
    const auto middle_same = [&] {
        int bad = 0;
        for (int y = 0; y < anygba::kScreenHeight; ++y) {
            for (int x = 0; x < anygba::kScreenWidth; ++x) bad += wide.row(y)[x + 8] != normal.row(y)[x];
        }
        return bad == 0;
    };
    const auto bg = [&](int x, int y) { return hd::expand(frame->lines[0].bg_color(tile_index(static_cast<unsigned>(((x % 8) + 8) % 8 + (y % 8) * 8)))); };

    renderer.render(*frame, cfg, wide);  // Map: the tile map wraps around (256 wide)
    CHECK(wide.width == 256);
    CHECK(middle_same());
    CHECK(wide.row(9)[0] == bg(-8, 9));  // column -8 is map column 248: tile 1 again

    frame->scene.layers[0] = hd::Extend::Hide;  // a HUD layer: backdrop at the sides
    renderer.render(*frame, cfg, wide);
    CHECK(middle_same());
    CHECK(wide.row(9)[0] == hd::expand(frame->lines[0].bg_color(0)));

    frame->scene.layers[0] = hd::Extend::Clamp;  // the edge column repeated
    renderer.render(*frame, cfg, wide);
    CHECK(middle_same());
    for (int x = 0; x < 8; ++x) CHECK(wide.row(9)[x] == normal.row(9)[0]);
    for (int x = 248; x < 256; ++x) CHECK(wide.row(9)[x] == normal.row(9)[239]);

    frame->scene.layers[0] = hd::Extend::Mirror;
    renderer.render(*frame, cfg, wide);
    CHECK(middle_same());
    for (int x = 0; x < 8; ++x) CHECK(wide.row(9)[7 - x] == normal.row(9)[x]);

    frame->scene.pillarbox = true;
    renderer.render(*frame, cfg, wide);
    CHECK(middle_same());
    CHECK(wide.row(9)[0] == 0xFF000000u);
    CHECK(wide.row(70)[255] == 0xFF000000u);

    // Map extensions from a provider replace what the wrapped map would show at the sides.
    frame->scene = {};
    hd::MapExtension& ext = frame->extension[0];
    ext.x0 = -2;
    ext.y0 = 0;
    ext.width = 2;
    ext.height = 20;
    ext.entries.assign(40, 0);  // tile 0: transparent
    renderer.render(*frame, cfg, wide);
    CHECK(middle_same());
    CHECK(wide.row(9)[0] == hd::expand(frame->lines[0].bg_color(0)));
    CHECK(wide.row(9)[255] == bg(247, 9));  // not covered by the extension: the map as before
}

TEST_CASE("video options: HD settings parse, serialize and step in the menu") {
    namespace video = anygba::video;
    video::Options o;
    CHECK_FALSE(video::apply_serialized(
        o, "internal-scale=4;layer-filter=xbrz4x;smooth-affine=false;hd-pack=false;hd-dump=true;ai=frame;ai-model=x4plus;"
           "widescreen=16:9"));
    CHECK(o.internal_scale == 4);
    CHECK(o.layer_filter == video::Scaler::Xbrz4x);
    CHECK_FALSE(o.smooth_affine);
    CHECK_FALSE(o.hd_pack);
    CHECK(o.hd_dump);
    CHECK(o.ai == video::Ai::Frame);
    CHECK(o.ai_model == video::AiModel::X4Plus);
    CHECK(o.widescreen == video::Widescreen::W16x9);
    CHECK(video::widescreen_extra(o.widescreen) == 22);
    video::Options back;
    CHECK_FALSE(video::apply_serialized(back, video::serialize(o)));
    CHECK(back == o);
    CHECK(video::apply_setting(o, "internal-scale", "9"));
    CHECK(video::apply_setting(o, "widescreen", "21:9"));
    CHECK(video::apply_setting(o, "ai", "on"));

    video::Options m;
    const auto items = video::menu_items(m);
    REQUIRE(items.size() == 17);
    CHECK(items[9].label == "Internal scale");
    CHECK(items[16].value == "off");
    video::menu_step(m, 9, -1);
    CHECK(m.internal_scale == video::kMaxInternalScale);  // wraps
    video::menu_step(m, 9, 1);
    CHECK(m.internal_scale == 1);
    video::menu_step(m, 14, 1);
    CHECK(m.ai == video::Ai::Frame);
    video::menu_step(m, 16, -1);
    CHECK(m.widescreen == video::Widescreen::W16x9);
}

// ---- AI ----

namespace {

void varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

std::vector<std::uint8_t> field(int number, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    varint(out, (static_cast<std::uint64_t>(number) << 3) | 2);
    varint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> field(int number, std::uint64_t value) {
    std::vector<std::uint8_t> out;
    varint(out, static_cast<std::uint64_t>(number) << 3);
    varint(out, value);
    return out;
}

std::vector<std::uint8_t> cat(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// ValueInfoProto "name" with a float tensor of shape (1, 3, 128, 128).
std::vector<std::uint8_t> image_value(const std::string& name) {
    std::vector<std::uint8_t> shape;
    for (const int d : {1, 3, 128, 128}) {
        const auto dim = field(1, field(1, static_cast<std::uint64_t>(d)));
        shape.insert(shape.end(), dim.begin(), dim.end());
    }
    const auto tensor = cat({field(1, 1), field(2, shape)});
    return cat({field(1, std::vector<std::uint8_t>(name.begin(), name.end())), field(2, field(1, tensor))});
}

std::size_t count(const std::vector<std::uint8_t>& bytes, const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i + s.size() <= bytes.size(); ++i) n += std::equal(s.begin(), s.end(), bytes.begin() + static_cast<std::ptrdiff_t>(i));
    return n;
}

}  // namespace

TEST_CASE("onnx patch: fixed 128x128 image inputs and outputs become any size") {
    const auto node = field(1, cat({field(1, std::vector<std::uint8_t>{'x'}), field(2, std::vector<std::uint8_t>{'y'})}));
    const auto graph = cat({node, field(11, image_value("image")), field(12, image_value("upscaled")),
                            field(13, image_value("hidden"))});
    const auto model = cat({field(1, 10), field(7, graph), field(8, cat({field(2, 21)}))});
    std::string error;
    const auto patched = hd::ai::make_size_dynamic(model, &error);
    REQUIRE(patched);
    CHECK(count(*patched, "height") == 2);
    CHECK(count(*patched, "width") == 2);
    CHECK(count(*patched, "hidden") == 0);  // recorded intermediate shapes dropped
    CHECK(count(*patched, "image") == 1);
    CHECK(patched->size() < model.size() + 64);
    CHECK_FALSE(hd::ai::make_size_dynamic({1, 2, 3}, &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("ai: offline upscale of a dump makes a 4x pack (ANYGBA_AI_TEST=1: downloads model)") {
    const char* enabled = std::getenv("ANYGBA_AI_TEST");
    if (!enabled || std::string(enabled) != "1") {
        MESSAGE("skipped: set ANYGBA_AI_TEST=1 (needs ONNX Runtime and downloads the model)");
        return;
    }
    const std::filesystem::path cache = hd::ai::default_cache(std::nullopt);
    std::string error;
    auto up = hd::ai::Upscaler::create(anygba::video::AiModel::GeneralX4v3, cache, hd::ai::Device::Auto, &error);
    REQUIRE_MESSAGE(up, error);
    MESSAGE("AI upscaler on ", up->device());
    const auto frame = synthetic_frame();
    hd::Renderer renderer;
    Image one;
    renderer.render(*frame, {}, one);
    Image out;
    const auto start = std::chrono::steady_clock::now();
    const int runs = 30;
    for (int i = 0; i < runs; ++i) up->upscale(one, out);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / runs;
    MESSAGE("general-x4v3, 240x160 -> 960x640: ", ms, " ms per frame");
    CHECK(out.width == 960);
    CHECK(out.height == 640);
    // A sprite with transparency keeps transparent corners and an opaque middle.
    Image sprite;
    sprite.resize(16, 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) sprite.row(y)[x] = (x >= 4 && x < 12 && y >= 4 && y < 12) ? 0xFF3060C0u : 0u;
    }
    up->upscale(sprite, out);
    CHECK(out.width == 64);
    CHECK((out.row(0)[0] >> 24) == 0);
    CHECK((out.row(32)[32] >> 24) == 255);
}
