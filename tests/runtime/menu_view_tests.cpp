#include "cheats/cheat_list.h"
#include "input_map.h"
#include "player/menu.h"
#include "support/png.h"
#include "support/temp_dir.h"
#include "ui/menu_assets.h"
#include "video/pipeline.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
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

// A GBA-sized picture with hard pixel edges (sky, sun, hills, bricks, a diagonal), so the
// pixel-art scalers have something to round off.
anygba::video::Image pixel_scene() {
    anygba::video::Image img;
    img.resize(240, 160);
    for (int y = 0; y < 160; ++y) {
        for (int x = 0; x < 240; ++x) {
            std::uint32_t c = y < 100 ? anygba::video::pack(110, 170, 235) : anygba::video::pack(70, 150, 80);
            const int dx = x - 186, dy = y - 34;
            if (dx * dx + dy * dy < 18 * 18) c = anygba::video::pack(255, 214, 74);
            if (y >= 100 - static_cast<int>(12 * std::sin(x / 19.0)) - 10 && y < 100) c = anygba::video::pack(50, 125, 64);
            if (y >= 76 && y < 92 && x >= 24 && x < 120 && ((x / 8 + y / 4) % 2 == 0 || y % 4 == 0)) c = anygba::video::pack(192, 112, 58);
            if (std::abs((x - 130) - (y - 110)) < 2 && x >= 130 && x < 180) c = anygba::video::pack(250, 250, 250);
            img.row(y)[x] = c;
        }
    }
    return img;
}

// What the player gives the Video preview (main.cpp update_preview): the plain picture and
// the filter chain with the current settings, at the size the game takes in a w x h window.
struct PreviewPictures {
    anygba::video::Pipeline plain, chain;
    anygba::video::Image frame = pixel_scene(), before, after;
    std::uint64_t version = 0;

    void attach(anyui::Frame& f, const anygba::video::Options& options, int w, int h) {
        anyui::Preview& pv = f.page.preview;
        if (!pv.shown) return;
        const anygba::video::Rect r = anygba::video::fit(w, h, 240, 160, false);
        before = plain.render(frame, 240, 160, r.w, r.h);
        chain.set_options(options);
        after = chain.render(frame, 240, 160, r.w, r.h);
        pv.before = {before.pixels.data(), before.width, before.height};
        pv.after = {after.pixels.data(), after.width, after.height};
        pv.version = ++version;
    }
};

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
    fx.video.scaler = anygba::video::Scaler::Xbrz4x;
    PreviewPictures pictures;
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
                anyui::Frame frame = fx.menu.view();
                REQUIRE(frame.tab == static_cast<int>(tab));
                CHECK(frame.page.preview.shown == (tab == 2));
                pictures.attach(frame, fx.video, w, h);
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
                anyui::Frame video = fx.menu.view();
                pictures.attach(video, fx.video, w, h);
                view.render(out, video, look, 1.f / 60);
                view.wheel(100);
                view.render(out, video, look, 1.f / 60);
                view.render(out, video, look, 1.f / 60);
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

namespace {

// One host frame with nothing held, then one holding `held` (edge detection needs both).
void press(anygba::player::Menu& menu, std::uint16_t held, bool shift = false) {
    menu.update({});
    anygba::player::MenuInput in;
    in.gba_held = held;
    in.shift = shift;
    menu.update(in);
}

constexpr std::uint16_t kSelect = 1u << 2, kRight = 1u << 4, kDown = 1u << 7;

}  // namespace

TEST_CASE("menu view: the Video preview's divider, loupe and descriptions") {
    Fixture fx;
    fx.video.scaler = anygba::video::Scaler::Xbrz4x;
    fx.video.effect = anygba::video::Effect::LcdGrid;
    PreviewPictures pictures;
    anyui::ViewOptions look;
    look.reduce_motion = true;
    anyui::MenuView view(anygba::ui::regular_font(), anygba::ui::bold_font());
    const anyui::Canvas backdrop = scene(960, 540);
    view.set_background(backdrop.data(), 960, 540, look);
    fx.menu.open();
    view.open();
    fx.menu.click({anyui::HitKind::Tab, 2});
    anyui::Canvas out;
    const auto draw = [&] {
        anyui::Frame f = fx.menu.view();
        pictures.attach(f, fx.video, 960, 540);
        view.render(out, f, look, 1.f / 60);
        return f;
    };
    anyui::Frame f = draw();
    REQUIRE(f.page.preview.shown);
    CHECK(f.hint == "A choose   Left/Right change   Select loupe   [ ] or LT/RT divider   L/R tabs   B close");
    CHECK(f.page.preview.title == "Graphics preset: Custom");  // xBRZ 4x at 1x without smoothing: no preset
    press(fx.menu, kDown);
    f = draw();
    CHECK(f.page.preview.title == "Filter: xbrz4x");
    CHECK(f.page.preview.before_label == "Before");
    const anyui::Rect pic = view.rect_of({anyui::HitKind::Preview, -1});
    REQUIRE(pic.w > 100);
    CHECK(pic.w / pic.h == doctest::Approx(1.5).epsilon(0.02));
    CHECK(view.hit(pic.x + 5, pic.y + 5) == anyui::Hit{anyui::HitKind::Preview, -1});

    // A mouse: dragging moves the divider, hovering moves the loupe; neither is a click.
    using Phase = anyui::MenuView::PointerPhase;
    anyui::PreviewState& st = fx.menu.preview_state();
    CHECK(view.preview_pointer(st, Phase::Down, pic.x + pic.w * 0.25f, pic.y + pic.h / 2, false));
    CHECK(st.split == doctest::Approx(0.25).epsilon(0.01));
    CHECK(view.preview_pointer(st, Phase::Move, pic.x + pic.w * 0.3f, pic.y + pic.h / 2, false));
    CHECK(st.split == doctest::Approx(0.3).epsilon(0.01));
    CHECK(view.preview_pointer(st, Phase::Up, pic.x + pic.w * 0.3f, pic.y + pic.h / 2, false));
    CHECK(!view.preview_pointer(st, Phase::Move, pic.x + pic.w * 0.6f, pic.y + pic.h * 0.4f, false));
    CHECK(st.pointer);
    CHECK(st.loupe_x == doctest::Approx(0.6).epsilon(0.01));
    CHECK(st.loupe_y == doctest::Approx(0.4).epsilon(0.01));
    CHECK(!view.preview_pointer(st, Phase::Down, pic.x - 40, pic.y, false));  // a row: the menu's click
    view.preview_pointer(st, Phase::Move, pic.x + pic.w * 0.32f, pic.y + pic.h * 0.45f, false);
    f = draw();
    check_golden("video-loupe-1x-dark", out);
    CHECK(f.page.preview.state.loupe_shown());
    view.preview_pointer(st, Phase::Move, pic.x - 40, pic.y, false);
    CHECK(!st.pointer);

    // A finger: near the divider it drags it; elsewhere it holds the loupe until lifted.
    const float divider = pic.x + st.split * pic.w;
    CHECK(view.preview_pointer(st, Phase::Down, divider + 4, pic.y + 10, true));
    CHECK(view.preview_pointer(st, Phase::Move, pic.x + pic.w * 0.8f, pic.y + 10, true));
    CHECK(st.split == doctest::Approx(0.8).epsilon(0.01));
    CHECK(!st.pointer);
    CHECK(view.preview_pointer(st, Phase::Up, pic.x + pic.w * 0.8f, pic.y + 10, true));
    CHECK(view.preview_pointer(st, Phase::Down, pic.x + 10, pic.y + pic.h - 10, true));
    CHECK(st.pointer);
    CHECK(st.split == doctest::Approx(0.8).epsilon(0.01));
    CHECK(view.preview_pointer(st, Phase::Up, pic.x + 10, pic.y + pic.h - 10, true));
    CHECK(!st.pointer);

    // Keys and the stick: [ ] and the triggers move the divider, Select toggles the loupe,
    // Shift+d-pad and the right stick move it; the d-pad alone still moves the cursor.
    anygba::player::MenuInput in;
    in.keys = {"["};
    fx.menu.update(in);
    CHECK(st.split == doctest::Approx(0.7).epsilon(0.01));
    in = {};
    in.pads = {anygba::input::Button::RightTrigger};
    fx.menu.update(in);
    CHECK(st.split == doctest::Approx(0.8).epsilon(0.01));
    CHECK(!st.loupe);
    press(fx.menu, kSelect);
    CHECK(st.loupe);
    const float lx = st.loupe_x;
    press(fx.menu, kRight, true);
    CHECK(st.loupe_x == doctest::Approx(lx + 0.05f));
    CHECK(fx.video.scaler == anygba::video::Scaler::Xbrz4x);  // Shift+Right moved the loupe, not the value
    in = {};
    in.stick_y = 1;
    const float ly = st.loupe_y;
    fx.menu.update(in);
    CHECK(st.loupe_y > ly);
    press(fx.menu, kRight);
    CHECK(fx.video.scaler != anygba::video::Scaler::Xbrz4x);
    press(fx.menu, kSelect);
    CHECK(!st.loupe);

    // Every Video row has a description, in English and Vietnamese.
    for (const char* language : {"en", "vi"}) {
        fx.mapping.language = language;
        fx.menu.click({anyui::HitKind::Tab, 2});
        const std::size_t count = fx.menu.view().page.rows.size();
        std::vector<std::string> seen;
        for (std::size_t i = 0; i < count; ++i) {
            const anyui::Preview pv = fx.menu.view().page.preview;
            CAPTURE(language);
            CAPTURE(pv.title);
            CHECK(!pv.description.empty());
            CHECK(!pv.description.starts_with("desc."));
            CHECK(std::find(seen.begin(), seen.end(), pv.description) == seen.end());
            seen.push_back(pv.description);
            press(fx.menu, kDown);
        }
        CHECK(seen.size() == count);
    }
    CHECK(fx.menu.view().page.preview.before_label == "Tr\xC6\xB0\xE1\xBB\x9B" "c");
}

TEST_CASE("anyui resample: area shrinking averages, nearest and linear enlarging") {
    // Black and white columns, one pixel each.
    std::vector<std::uint32_t> stripes(8 * 2);
    for (int i = 0; i < 16; ++i) stripes[static_cast<std::size_t>(i)] = i % 2 ? 0xFFFFFFFFu : 0xFF000000u;
    std::vector<std::uint32_t> out(4);
    anyui::resample(stripes.data(), 8, 2, out.data(), 4, 1, anyui::Resample::Area);
    for (const std::uint32_t p : out) CHECK((p & 0xFF) == 128);
    anyui::resample(stripes.data(), 8, 2, out.data(), 4, 1, anyui::Resample::Nearest);
    for (const std::uint32_t p : out) CHECK((p & 0xFF) == 255);  // every second column: aliasing

    const std::uint32_t two[2] = {0xFF000000u, 0xFFC8C8C8u};
    std::vector<std::uint32_t> big(8);
    anyui::resample(two, 2, 1, big.data(), 8, 1, anyui::Resample::Area);  // enlarging: nearest
    CHECK(big == std::vector<std::uint32_t>{two[0], two[0], two[0], two[0], two[1], two[1], two[1], two[1]});
    anyui::resample(two, 2, 1, big.data(), 4, 1, anyui::Resample::Linear);
    CHECK((big[0] & 0xFF) == 0);
    CHECK((big[1] & 0xFF) == 50);
    CHECK((big[2] & 0xFF) == 150);
    CHECK((big[3] & 0xFF) == 200);
}

