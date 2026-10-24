#include "cheats/cheat_list.h"
#include "input_map.h"
#include "player/menu.h"
#include "support/png.h"
#include "support/temp_dir.h"
#include "ui/menu_assets.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// The menu drawn offscreen: golden pictures of every tab at 1x (960x540) and 2x
// (1920x1080) in the dark and light themes, pointer hits, and the open/close animation.
// ANYGBA_UPDATE_GOLDENS=1 rewrites tests/golden/menu; ANYGBA_MENU_SHOTS=<dir> also writes
// every rendered picture there.
namespace {

namespace fs = std::filesystem;
const fs::path kGolden = fs::path(ANYGBA_GOLDEN_DIR) / "menu";

constexpr const char* kCheats = R"(
[game]
title = "test"

[lists]
items = ["1 Potion", "2 Ether", "0x10 Elixir"]

[[cheat]]
name = "Items/Infinite money"
format = "raw"
code = ["0203F000:0000FFFF"]

[[cheat]]
name = "Items/Pick an item"
format = "cb"
code = ["8203F020 {v:4}"]
param = { label = "Item", list = "items" }

[[cheat]]
name = "Battle/Walk through walls"
format = "raw"
code = ["0203F010:00000001"]
)";

// A made-up game scene: sky, sun, hills and a few blocks, so the frosted backdrop shows.
anyui::Canvas scene(int w, int h) {
    anyui::Canvas c(w, h);
    for (int y = 0; y < h; ++y) {
        const float t = static_cast<float>(y) / static_cast<float>(h);
        const std::uint32_t px = t < 0.5f ? anyui::pack(0.45f, 0.68f, 0.92f) : anyui::pack(0.55f, 0.75f, 0.9f);
        std::fill(c.row(y), c.row(y) + w, px);
    }
    const float u = static_cast<float>(h) / 160.f;
    c.fill_circle(190 * u, 30 * u, 16 * u, anyui::Color::rgb(0xFFD54A));
    c.fill_round_rect({-20 * u, 110 * u, 160 * u, 120 * u}, 60 * u, anyui::Color::rgb(0x3E9B4F));
    c.fill_round_rect({90 * u, 120 * u, 220 * u, 120 * u}, 60 * u, anyui::Color::rgb(0x2F7F40));
    for (int i = 0; i < 4; ++i) {
        c.fill_rect({(30.f + 40.f * static_cast<float>(i)) * u, 90 * u, 16 * u, 16 * u}, anyui::Color::rgb(0xC0703A));
    }
    return c;
}

std::vector<std::uint8_t> to_rgb(const anyui::Canvas& c) {
    std::vector<std::uint8_t> rgb;
    rgb.reserve(c.pixels().size() * 3);
    for (const std::uint32_t p : c.pixels()) {
        rgb.push_back(static_cast<std::uint8_t>(p & 0xFF));
        rgb.push_back(static_cast<std::uint8_t>((p >> 8) & 0xFF));
        rgb.push_back(static_cast<std::uint8_t>((p >> 16) & 0xFF));
    }
    return rgb;
}

// Half size, each pixel the mean of a 2x2 block: 2x pictures are kept at 1x size so the
// goldens stay small; they still differ from the 1x ones (text and lines drawn at 2x).
anyui::Canvas halve(const anyui::Canvas& c) {
    anyui::Canvas h(c.width() / 2, c.height() / 2);
    for (int y = 0; y < h.height(); ++y) {
        for (int x = 0; x < h.width(); ++x) {
            unsigned sum[3] = {0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t p = c.row(2 * y + k / 2)[2 * x + k % 2];
                for (int ch = 0; ch < 3; ++ch) sum[ch] += (p >> (8 * ch)) & 0xFF;
            }
            h.row(y)[x] = (sum[0] + 2) / 4 | ((sum[1] + 2) / 4) << 8 | ((sum[2] + 2) / 4) << 16 | 0xFF000000u;
        }
    }
    return h;
}

// Mean squared error of at most 1 per channel against the golden picture (text and
// curves may round differently on other compilers).
void check_golden(const std::string& name, const anyui::Canvas& full) {
    if (const char* shots = std::getenv("ANYGBA_MENU_SHOTS"); shots && *shots) {
        fs::create_directories(shots);
        anygba::test::write_png_rgb(fs::path(shots) / (name + ".png"), to_rgb(full), full.width(), full.height());
    }
    const anyui::Canvas c = full.width() > 1000 ? halve(full) : full;
    const std::vector<std::uint8_t> actual = to_rgb(c);
    const fs::path path = kGolden / (name + ".png");
    if (const char* update = std::getenv("ANYGBA_UPDATE_GOLDENS"); update && std::string(update) == "1") {
        fs::create_directories(kGolden);
        anygba::test::write_png_rgb(path, actual, c.width(), c.height());
        MESSAGE("wrote " << path.string());
        return;
    }
    REQUIRE_MESSAGE(fs::exists(path), "missing golden " << path.string() << " (run with ANYGBA_UPDATE_GOLDENS=1)");
    const std::vector<std::uint8_t> expected = anygba::test::read_png_rgb(path, c.width(), c.height());
    double sum = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const double d = static_cast<double>(actual[i]) - expected[i];
        sum += d * d;
    }
    const double mse = sum / static_cast<double>(actual.size());
    if (mse != 0) {
        const auto out = fs::absolute("menu-golden-" + name + ".png");
        anygba::test::write_png_rgb(out, actual, c.width(), c.height());
        MESSAGE(name << ": MSE " << mse << " against the golden; actual written to " << out.string());
    }
    CHECK_MESSAGE(mse <= 1.0, name << " differs from its golden picture (MSE " << mse << ")");
}

struct Fixture {
    anygba::test::TempDir dir{"anygba-menu-view"};
    anygba::cheats::CheatList cheats;
    anygba::input::Mapping mapping = anygba::input::default_mapping();
    int speed = 100, slot = 2;
    anygba::video::Options video;
    anygba::player::MenuSettings ai;
    anygba::player::Menu menu{cheats, mapping, speed, slot};

    Fixture() {
        std::ofstream(dir.path / "db.toml", std::ios::binary) << kCheats;
        REQUIRE(cheats.load_database(dir.path / "db.toml", 0));
        menu.set_video(&video);
        menu.set_settings(&ai);
        menu.set_game_title("Test Quest Advance");
        std::array<anygba::player::SlotInfo, anygba::player::kStateSlots> slots{};
        const anyui::Canvas thumb = scene(240, 160);
        for (int i : {0, 1, 4}) {
            auto& s = slots[static_cast<std::size_t>(i)];
            s.used = true;
            s.when = "2026-10-2" + std::to_string(i) + " 21:0" + std::to_string(i);
            s.thumb = thumb.pixels();
            s.thumb_width = 240;
            s.thumb_height = 160;
        }
        menu.set_slots(slots);
    }
};

}  // namespace

TEST_CASE("menu font: Vietnamese letters, metrics and fitting") {
    const anyui::Font& f = anygba::ui::regular_font();
    for (const char32_t cp : {U'A', U'ặ', U'ư', U'đ', U'…'}) {
        CAPTURE(static_cast<unsigned>(cp));
        CHECK(f.glyph(cp) != 0);
    }
    CHECK(f.measure("Cài đặt", 20) > 40);
    CHECK(f.line_height(20) > 20);
    const std::string cut = f.fit("A long label that does not fit", 16, 80);
    CHECK(cut.ends_with("\xE2\x80\xA6"));
    CHECK(f.measure(cut, 16) <= 80);
    const auto& b = f.bitmap(f.glyph(U'ặ'), 32);  // ặ: two marks, above and below
    CHECK(b.height > 28);
    CHECK(anygba::ui::bold_font().measure("AnyGBA", 20) > f.measure("AnyGBA", 20) * 0.98f);
}

TEST_CASE("menu view: golden pictures of every tab at 1x and 2x, dark and light") {
    Fixture fx;
    const std::array<const char*, 8> names = {"game", "states", "video", "audio", "controls", "cheats", "ai", "settings"};
    for (const int scale : {1, 2}) {
        const int w = 960 * scale, h = 540 * scale;
        const anyui::Canvas backdrop = scene(w, h);
        for (const bool dark : {true, false}) {
            anyui::ViewOptions look;
            look.dark = dark;
            look.reduce_motion = true;
            anyui::MenuView view(anygba::ui::regular_font(), anygba::ui::bold_font());
            view.set_background(backdrop.data(), w, h, look);
            fx.menu.open();
            view.open();
            anyui::Canvas out;
            for (std::size_t tab = 0; tab < names.size(); ++tab) {
                fx.menu.click({anyui::HitKind::Tab, static_cast<int>(tab)});
                const anyui::Frame frame = fx.menu.view();
                REQUIRE(frame.tab == static_cast<int>(tab));
                view.render(out, frame, look, 1.f / 60);
                CHECK(out.width() == w);
                check_golden(std::string(names[tab]) + "-" + std::to_string(scale) + "x-" + (dark ? "dark" : "light"), out);
            }
            // A slot's actions and a rebinding prompt, in the dark theme at 1x.
            if (dark && scale == 1) {
                fx.menu.click({anyui::HitKind::Tab, 1});
                fx.menu.click({anyui::HitKind::Slot, 1});
                view.render(out, fx.menu.view(), look, 1.f / 60);
                check_golden("states-popup-1x-dark", out);
                fx.menu.click({anyui::HitKind::Tab, 4});
                const auto rows = fx.menu.view().page.rows;
                fx.menu.click({anyui::HitKind::Row, 2});  // B
                view.render(out, fx.menu.view(), look, 1.f / 60);
                CHECK(fx.menu.view().page.modal == "Press a key or button for B");
                check_golden("controls-capture-1x-dark", out);
                fx.menu.click({anyui::HitKind::Outside, -1});
                // The display timing rows at the end of the Video page.
                fx.menu.click({anyui::HitKind::Tab, 2});
                std::vector<std::string> labels;
                for (const auto& row : fx.menu.view().page.rows) labels.push_back(row.label);
                for (const char* want : {"Smoothing", "Refresh", "Variable refresh", "Sync game to monitor", "Menu FPS", "FPS cap"}) {
                    CAPTURE(want);
                    CHECK(std::find(labels.begin(), labels.end(), want) != labels.end());
                }
                view.render(out, fx.menu.view(), look, 1.f / 60);
                view.wheel(100);
                view.render(out, fx.menu.view(), look, 1.f / 60);
                view.render(out, fx.menu.view(), look, 1.f / 60);
                check_golden("video-end-1x-dark", out);
            }
        }
    }
}

TEST_CASE("menu view: clicks land on what was drawn, in Vietnamese too") {
    Fixture fx;
    fx.mapping.language = "vi";
    anyui::ViewOptions look;
    look.reduce_motion = true;
    anyui::MenuView view(anygba::ui::regular_font(), anygba::ui::bold_font());
    const anyui::Canvas backdrop = scene(1280, 720);
    view.set_background(backdrop.data(), 1280, 720, look);
    fx.menu.open();
    view.open();
    anyui::Canvas out;
    view.render(out, fx.menu.view(), look, 1.f / 60);
    CHECK(fx.menu.view().tabs[3].label == "\xC3\x82m thanh");

    // The Audio tab, through the rail.
    const anyui::Rect tab = view.rect_of({anyui::HitKind::Tab, 3});
    REQUIRE(tab.w > 0);
    CHECK(view.hit(tab.x + tab.w / 2, tab.y + tab.h / 2) == anyui::Hit{anyui::HitKind::Tab, 3});
    fx.menu.click(view.hit(tab.x + tab.w / 2, tab.y + tab.h / 2));
    view.render(out, fx.menu.view(), look, 1.f / 60);
    CHECK(fx.menu.lines()[0] == "== \xC3\x82m thanh ==");

    // The volume slider's left part lowers it.
    const anyui::Rect lower = view.rect_of({anyui::HitKind::Decrease, 0});
    REQUIRE(lower.w > 0);
    fx.menu.click(view.hit(lower.x + 2, lower.y + lower.h / 2));
    CHECK(fx.mapping.volume == 90);
    view.render(out, fx.menu.view(), look, 1.f / 60);

    // The mute switch's row.
    const anyui::Rect mute = view.rect_of({anyui::HitKind::Row, 1});
    fx.menu.click(view.hit(mute.x + 20, mute.y + mute.h / 2));
    CHECK(fx.mapping.mute);

    // Outside the card closes the menu.
    const auto actions = fx.menu.click(view.hit(2, 2));
    CHECK(actions == std::vector<anygba::player::MenuAction>{anygba::player::MenuAction::Close});
    CHECK(!fx.menu.is_open());
}

TEST_CASE("menu view: opening and closing animate unless motion is reduced") {
    Fixture fx;
    anyui::ViewOptions look;
    const anyui::Canvas backdrop = scene(480, 270);
    anyui::MenuView view(anygba::ui::regular_font(), anygba::ui::bold_font());
    view.set_background(backdrop.data(), 480, 270, look);
    fx.menu.open();
    view.open();
    anyui::Canvas out;
    view.render(out, fx.menu.view(), look, 1.f / 60);
    CHECK(view.animating());
    const std::vector<std::uint32_t> first = out.pixels();
    for (int i = 0; i < 30; ++i) view.render(out, fx.menu.view(), look, 1.f / 60);
    CHECK(!view.animating());
    CHECK(out.pixels() != first);
    view.close();
    int frames = 0;
    while (!view.closed() && frames < 60) {
        view.render(out, fx.menu.view(), look, 1.f / 60);
        ++frames;
    }
    CHECK(view.closed());
    CHECK(frames > 3);
    CHECK(out.pixels() == std::vector<std::uint32_t>(backdrop.pixels()));  // closed: the game picture again

    look.reduce_motion = true;
    view.open();
    view.render(out, fx.menu.view(), look, 1.f / 60);
    CHECK(!view.animating());
    view.close();
    view.render(out, fx.menu.view(), look, 1.f / 60);
    CHECK(view.closed());
}
