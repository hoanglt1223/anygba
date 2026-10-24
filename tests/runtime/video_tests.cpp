#include "anygba/video_options.h"
#include "cheats/cheat_list.h"
#include "player/menu.h"
#include "support/png.h"
#include "support/temp_dir.h"
#include "video/color.h"
#include "video/pipeline.h"
#include "video/presenter.h"
#include "video/scalers.h"
#include "video/settings.h"

#include <anyport/platform/context.h>
#include <anyport/platform/timing.h>
#include <anyport/platform/window.h>

#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace video = anygba::video;
namespace pf = anyport::platform;

namespace {

const std::filesystem::path kGolden = std::filesystem::path(ANYGBA_GOLDEN_DIR) / "video";

constexpr std::uint16_t rgb555(unsigned r, unsigned g, unsigned b) {
    return static_cast<std::uint16_t>(r | (g << 5) | (b << 10));
}

// A self-made test picture with what scalers care about: a smooth ramp, lines of several
// slopes, a disc, a checkerboard, a solid block, single pixels and thin text-like strokes.
std::vector<std::uint16_t> test_frame(int w, int h) {
    std::vector<std::uint16_t> f(static_cast<std::size_t>(w * h));
    const auto set = [&](int x, int y, std::uint16_t c) {
        if (x >= 0 && y >= 0 && x < w && y < h) f[static_cast<std::size_t>(y * w + x)] = c;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) set(x, y, rgb555(4, 6 + y * 8 / h, static_cast<unsigned>(8 + x * 16 / w)));
    }
    for (int i = 0; i < h; ++i) set(i, i, rgb555(31, 31, 31));              // 45 degrees
    for (int i = 0; i < w; ++i) set(i, h - 1 - i / 2, rgb555(31, 31, 0));    // shallow
    for (int i = 0; i < h; ++i) set(w - 1 - i / 3, i, rgb555(0, 31, 31));    // steep
    const int cx = w * 3 / 4, cy = h / 3, r = std::max(2, h / 5);
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            if (x * x + y * y <= r * r) set(cx + x, cy + y, rgb555(31, 4, 4));
        }
    }
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 6; ++x) set(2 + x, h - 8 + y, ((x + y) & 1) ? rgb555(0, 0, 0) : rgb555(31, 31, 31));
    }
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) set(w / 2 + x, 2 + y, rgb555(8, 24, 8));
    }
    set(w / 2 - 3, h / 2, rgb555(31, 0, 31));
    set(w / 2 + 3, h / 2 + 2, rgb555(0, 0, 0));
    // An "A"-like glyph, one pixel thick.
    for (int i = 0; i < 6; ++i) {
        set(4 + i / 2, 9 - i, rgb555(0, 0, 0));
        set(9 - i / 2, 9 - i, rgb555(0, 0, 0));
    }
    for (int x = 5; x <= 8; ++x) set(x, 7, rgb555(0, 0, 0));
    return f;
}

std::vector<std::uint8_t> to_rgb(const video::Image& image) {
    std::vector<std::uint8_t> rgb;
    rgb.reserve(image.pixels.size() * 3);
    for (const std::uint32_t p : image.pixels) {
        rgb.push_back(static_cast<std::uint8_t>(video::red(p)));
        rgb.push_back(static_cast<std::uint8_t>(video::green(p)));
        rgb.push_back(static_cast<std::uint8_t>(video::blue(p)));
    }
    return rgb;
}

// Exact match, or a mean squared error of at most 1 per channel (compiler floating-point
// differences in the colour tables). ANYGBA_UPDATE_GOLDENS=1 rewrites the files instead.
void check_golden(const std::string& name, const video::Image& image) {
    const std::filesystem::path path = kGolden / (name + ".png");
    const std::vector<std::uint8_t> actual = to_rgb(image);
    if (const char* update = std::getenv("ANYGBA_UPDATE_GOLDENS"); update && std::string(update) == "1") {
        std::filesystem::create_directories(kGolden);
        anygba::test::write_png_rgb(path, actual, image.width, image.height);
        MESSAGE("wrote " << path.string());
        return;
    }
    REQUIRE_MESSAGE(std::filesystem::exists(path), "missing golden " << path.string()
                                                                      << " (run with ANYGBA_UPDATE_GOLDENS=1)");
    const std::vector<std::uint8_t> expected = anygba::test::read_png_rgb(path, image.width, image.height);
    double sum = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const double d = static_cast<double>(actual[i]) - expected[i];
        sum += d * d;
    }
    const double mse = sum / static_cast<double>(actual.size());
    if (mse != 0) {
        const auto out = std::filesystem::absolute("video-golden-" + name + ".png");
        anygba::test::write_png_rgb(out, actual, image.width, image.height);
        MESSAGE(name << ": MSE " << mse << " against the golden; actual written to " << out.string());
    }
    CHECK_MESSAGE(mse <= 1.0, name << " differs from its golden picture (MSE " << mse << ")");
}

// Frame-time budgets are only checked in optimised builds run alone (ctest video-timing sets
// ANYGBA_VIDEO_TIMING=1 and runs serially); otherwise the timings are just reported.
bool timing_checked() {
#ifdef NDEBUG
    // Shared CI runners are too slow and noisy for a frame budget; keep it local.
    if (std::getenv("CI")) return false;
    const char* v = std::getenv("ANYGBA_VIDEO_TIMING");
    return v && std::string(v) == "1";
#else
    return false;
#endif
}

video::Image source_image(int w, int h) {
    const video::ColorConverter raw;
    video::Image img;
    raw.convert(test_frame(w, h), w, h, img);
    return img;
}

}  // namespace

TEST_CASE("video filter specs parse, reject mistakes and round-trip") {
    video::Options o;
    CHECK_FALSE(video::parse_filter("hq3x+lcd-grid+ghosting", o));
    CHECK(o.scaler == video::Scaler::Hq3x);
    CHECK(o.effect == video::Effect::LcdGrid);
    CHECK(o.ghosting);
    CHECK(video::filter_spec(o) == "hq3x+lcd-grid+ghosting");
    CHECK_FALSE(video::parse_filter("crt", o));
    CHECK(o.scaler == video::Scaler::Nearest);
    CHECK(o.effect == video::Effect::Crt);
    CHECK_FALSE(o.ghosting);
    CHECK(video::parse_filter("hq3x+xbrz4x", o));
    CHECK(video::parse_filter("crt+scanlines", o));
    CHECK(video::parse_filter("hq5x", o));
    CHECK(video::parse_filter("", o));
    for (const video::Scaler s : video::all_scalers()) {
        video::Options p;
        REQUIRE_FALSE(video::parse_filter(video::name(s), p));
        CHECK(p.scaler == s);
    }

    video::Options full;
    CHECK_FALSE(video::apply_setting(full, "filter", "xbrz6x+crt"));
    CHECK_FALSE(video::apply_setting(full, "color", "gba-lcd"));
    CHECK_FALSE(video::apply_setting(full, "gamma", "1.2"));
    CHECK_FALSE(video::apply_setting(full, "deband", "on"));
    CHECK_FALSE(video::apply_setting(full, "output", "1440p"));
    CHECK_FALSE(video::apply_setting(full, "fullscreen", "true"));
    CHECK_FALSE(video::apply_setting(full, "integer-scale", "1"));
    CHECK(video::apply_setting(full, "gamma", "3"));
    CHECK(video::apply_setting(full, "gamma", "1.2x"));
    CHECK(video::apply_setting(full, "color", "sepia"));
    CHECK(video::apply_setting(full, "deband", "maybe"));
    CHECK(video::apply_setting(full, "brightness", "1"));
    const std::string text = video::serialize(full);
    CHECK(text == "filter=xbrz6x+crt;color=gba-lcd;gamma=1.2;deband=true;output=1440p;fullscreen=true;integer-scale=true");
    video::Options back;
    CHECK_FALSE(video::apply_serialized(back, text));
    CHECK(back == full);
    CHECK(video::serialize(video::Options{}).empty());
    CHECK(video::apply_serialized(back, "filter"));
}

TEST_CASE("video settings: game config, saved file and command line in that order") {
    anygba::test::TempDir dir("anygba-video-settings");
    const auto file = dir.path / "game.video";
    std::string warning;
    video::Options o = video::resolve_settings("filter=hq2x;color=gba-lcd", file, "", &warning);
    CHECK(o.scaler == video::Scaler::Hq2x);
    CHECK(o.color == video::Color::GbaLcd);
    CHECK(warning.empty());

    o.scaler = video::Scaler::Xbrz3x;
    o.integer_scale = true;
    REQUIRE_FALSE(video::save_settings(file, o));
    o = video::resolve_settings("filter=hq2x", file, "", &warning);
    CHECK(o.scaler == video::Scaler::Xbrz3x);
    CHECK(o.integer_scale);
    o = video::resolve_settings("filter=hq2x", file, "filter=eagle;gamma=0.8", &warning);
    CHECK(o.scaler == video::Scaler::Eagle);
    CHECK(o.gamma == 0.8);

    CHECK(video::resolve_settings(nullptr, std::nullopt, "", nullptr) == video::Options{});
    CHECK_THROWS_WITH(video::resolve_settings("", std::nullopt, "filter=hq9x", nullptr), doctest::Contains("hq9x"));
    CHECK_THROWS_WITH(video::resolve_settings("color=blue", std::nullopt, "", nullptr), doctest::Contains("[video]"));
    {
        std::ofstream(file, std::ios::binary | std::ios::trunc) << "filter=nope\n";
    }
    o = video::resolve_settings("filter=hq2x", file, "", &warning);
    CHECK(o.scaler == video::Scaler::Hq2x);
    CHECK(warning.find("ignored") != std::string::npos);
}

TEST_CASE("menu: the Video page steps every setting and reports the change") {
    anygba::cheats::CheatList list;
    anygba::input::Mapping mapping = anygba::input::default_mapping();
    int speed = 100;
    int slot = 1;
    video::Options options;
    anygba::player::Menu menu(list, mapping, speed, slot);
    menu.set_video(&options);
    using anygba::player::MenuAction;
    constexpr std::uint16_t A = 1, B = 2, Right = 16, Left = 32, Up = 64, Down = 128;
    std::vector<MenuAction> actions;
    const auto press = [&](std::uint16_t buttons) {
        menu.update({});
        anygba::player::MenuInput in;
        in.gba_held = buttons;
        auto a = menu.update(in);
        actions.insert(actions.end(), a.begin(), a.end());
    };
    menu.open();
    menu.update({});
    press(Up);  // wraps to the last row, Options
    press(Up);  // Video
    press(A);
    REQUIRE(menu.lines()[0] == "== Video ==");
    CHECK(menu.lines()[1] == "> Filter  < nearest >");
    press(Right);
    CHECK(options.scaler == video::Scaler::Bilinear);
    press(Left);
    press(Left);
    CHECK(options.scaler == video::Scaler::Xbrz6x);
    press(Down);
    press(Right);
    CHECK(options.effect == video::Effect::LcdGrid);
    press(Down);
    press(A);
    CHECK(options.ghosting);
    press(Down);
    press(Right);
    CHECK(options.color == video::Color::GbaLcd);
    press(Down);
    press(Right);
    CHECK(options.gamma == doctest::Approx(1.1));
    press(Left);
    press(Left);
    CHECK(options.gamma == doctest::Approx(0.9));
    press(Down);
    press(Right);
    CHECK(options.deband);
    press(Down);
    press(Left);
    CHECK(options.output == video::Output::Desktop);
    press(Down);
    press(Right);
    CHECK(options.fullscreen);
    press(Down);
    press(Right);
    CHECK(options.integer_scale);
    CHECK(std::count(actions.begin(), actions.end(), MenuAction::VideoChanged) == 13);
    press(B);
    CHECK(menu.lines()[0] == "== AnyGBA ==");
}

TEST_CASE("the picture keeps 3:2 inside any output, optionally at whole multiples") {
    CHECK(video::fit(1920, 1080, 240, 160, false) == video::Rect{150, 0, 1620, 1080});
    CHECK(video::fit(3840, 2160, 240, 160, false) == video::Rect{300, 0, 3240, 2160});
    CHECK(video::fit(1280, 720, 240, 160, true) == video::Rect{160, 40, 960, 640});
    CHECK(video::fit(3840, 2160, 240, 160, true) == video::Rect{360, 40, 3120, 2080});
    CHECK(video::fit(240, 400, 240, 160, false) == video::Rect{0, 120, 240, 160});
    CHECK(video::fit(100, 100, 240, 160, true) == video::Rect{0, 0, 100, 100});
    CHECK_THROWS(video::fit(0, 10, 240, 160, false));
}

TEST_CASE("colour: raw is exact, GBA LCD is darker and desaturated, gamma and debanding work") {
    const video::ColorConverter raw;
    CHECK(raw.convert(rgb555(31, 31, 31)) == video::pack(255, 255, 255));
    CHECK(raw.convert(rgb555(16, 1, 0)) == video::pack(132, 8, 0));
    CHECK(raw.convert(0x8000u | rgb555(31, 0, 0)) == video::pack(255, 0, 0));  // bit 15 ignored

    video::Options lcd;
    lcd.color = video::Color::GbaLcd;
    const video::ColorConverter gba(lcd);
    const std::uint32_t white = gba.convert(rgb555(31, 31, 31));
    CHECK(video::red(white) < 255);
    CHECK(video::red(white) > 200);
    const std::uint32_t red = gba.convert(rgb555(31, 0, 0));
    CHECK(video::green(red) > 0);  // primaries bleed
    CHECK(video::red(gba.convert(rgb555(16, 16, 16))) < video::red(raw.convert(rgb555(16, 16, 16))));

    video::Options bright;
    bright.gamma = 2.0;
    CHECK(video::red(video::ColorConverter(bright).convert(rgb555(16, 16, 16))) >
          video::red(raw.convert(rgb555(16, 16, 16))));

    // A ramp of one-level steps gains intermediate shades; a hard edge stays hard.
    constexpr int kW = 64, kH = 4;
    std::vector<std::uint16_t> ramp(kW * kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) ramp[static_cast<std::size_t>(y * kW + x)] = rgb555(0, 0, x < 48 ? x / 3 : 31);
    }
    video::Options deband;
    deband.deband = true;
    video::Image plain, smooth;
    raw.convert(ramp, kW, kH, plain);
    video::ColorConverter(deband).convert(ramp, kW, kH, smooth);
    std::set<unsigned> plain_levels, smooth_levels;
    for (int x = 0; x < 46; ++x) {
        plain_levels.insert(video::blue(plain.row(1)[x]));
        smooth_levels.insert(video::blue(smooth.row(1)[x]));
    }
    CHECK(smooth_levels.size() > plain_levels.size() + 10);
    CHECK(smooth.row(1)[50] == plain.row(1)[50]);
    CHECK(video::blue(smooth.row(1)[47]) == video::blue(plain.row(1)[47]));
}

TEST_CASE("every scaler keeps flat colour flat and produces its factor") {
    video::Image flat;
    flat.resize(9, 7);
    std::fill(flat.pixels.begin(), flat.pixels.end(), video::pack(10, 200, 30));
    for (const video::Scaler s : video::all_scalers()) {
        CAPTURE(video::name(s));
        video::Image out;
        video::upscale(s, flat, out);
        CHECK(out.width == 9 * video::factor(s));
        CHECK(out.height == 7 * video::factor(s));
        CHECK(std::all_of(out.pixels.begin(), out.pixels.end(), [](std::uint32_t p) { return p == video::pack(10, 200, 30); }));
    }
}

TEST_CASE("Scale2x rounds a diagonal step the documented way") {
    // B (above) == D (left) != E: the top-left output pixel takes D, the others stay E.
    video::Image src;
    src.resize(3, 3);
    const std::uint32_t k = video::pack(0, 0, 0), w = video::pack(255, 255, 255);
    src.pixels = {k, k, w, k, w, w, w, w, w};
    video::Image out;
    video::upscale(video::Scaler::Scale2x, src, out);
    CHECK(out.row(2)[2] == k);
    CHECK(out.row(2)[3] == w);
    CHECK(out.row(3)[2] == w);
    CHECK(out.row(3)[3] == w);
}

TEST_CASE("pixel-art scalers match their golden pictures") {
    const video::Image src = source_image(48, 32);
    for (const video::Scaler s : video::all_scalers()) {
        if (video::factor(s) == 1) continue;
        CAPTURE(video::name(s));
        video::Image out;
        video::upscale(s, src, out);
        check_golden(std::string(video::name(s)), out);
    }
}

TEST_CASE("filter chains match their golden pictures") {
    constexpr int kW = 48, kH = 32;
    const std::vector<std::uint16_t> frame = test_frame(kW, kH);
    // An output that is not a whole multiple of the picture exercises the final bilinear pass.
    const std::vector<std::pair<std::string, std::string>> chains = {
        {"nearest", "filter=nearest"},
        {"bilinear", "filter=bilinear"},
        {"sharp-bilinear", "filter=sharp-bilinear"},
        {"lcd-grid", "filter=nearest+lcd-grid;integer-scale=true"},
        {"scanlines", "filter=nearest+scanlines;integer-scale=true"},
        {"crt", "filter=nearest+crt;integer-scale=true"},
        {"hq2x-lcd-grid", "filter=hq2x+lcd-grid"},
        {"gba-lcd", "filter=nearest;color=gba-lcd"},
        {"gamma", "filter=nearest;gamma=1.6"},
        {"deband", "filter=bilinear;deband=true"},
    };
    for (const auto& [name, settings] : chains) {
        CAPTURE(name);
        video::Options o;
        REQUIRE_FALSE(video::apply_serialized(o, settings));
        video::Pipeline pipeline(o);
        check_golden("chain-" + name, pipeline.render(frame, kW, kH, 200, 120));
    }
}

TEST_CASE("ghosting blends each frame with the one before") {
    constexpr int kW = 32, kH = 32;
    std::vector<std::uint16_t> black(kW * kH, 0), white(kW * kH, rgb555(31, 31, 31));
    video::Options o;
    REQUIRE_FALSE(video::parse_filter("nearest+ghosting", o));
    video::Pipeline pipeline(o);
    CHECK(pipeline.render(black, kW, kH, kW, kH).pixels[0] == video::pack(0, 0, 0));
    CHECK(pipeline.render(white, kW, kH, kW, kH).pixels[0] == video::pack(128, 128, 128));
    CHECK(pipeline.render(white, kW, kH, kW, kH).pixels[0] == video::pack(255, 255, 255));
    o.ghosting = false;
    pipeline.set_options(o);
    CHECK(pipeline.render(black, kW, kH, kW, kH).pixels[0] == video::pack(0, 0, 0));
}

TEST_CASE("every filter renders a 4K frame within the frame budget on the CPU") {
    constexpr int kW = 240, kH = 160;
    const std::vector<std::uint16_t> frame = test_frame(kW, kH);
    std::vector<std::string> chains;
    for (const video::Scaler s : video::all_scalers()) chains.emplace_back(video::name(s));
    chains.insert(chains.end(), {"nearest+lcd-grid", "nearest+scanlines", "nearest+crt", "nearest+ghosting",
                                 "xbrz6x+crt+ghosting", "hq4x+lcd-grid"});
    double worst = 0;
    std::string worst_name;
    for (const auto& spec : chains) {
        video::Options o;
        REQUIRE_FALSE(video::parse_filter(spec, o));
        o.color = video::Color::GbaLcd;
        o.deband = true;
        video::Pipeline pipeline(o);
        pipeline.render(frame, kW, kH, 3840, 2160);  // warm-up: allocations, tables, threads
        double best = 1e9;
        for (int i = 0; i < 5; ++i) {
            const std::uint64_t t0 = pf::now_ns();
            const video::Image& out = pipeline.render(frame, kW, kH, 3840, 2160);
            best = std::min(best, static_cast<double>(pf::now_ns() - t0) / 1e6);
            REQUIRE(out.width == 3840);
        }
        MESSAGE(spec << ": " << best << " ms for 3840x2160");
        if (best > worst) {
            worst = best;
            worst_name = spec;
        }
    }
    MESSAGE("slowest: " << worst_name << " at " << worst << " ms");
    if (timing_checked()) {
        CHECK_MESSAGE(worst < 1000.0 / 60.0, worst_name << " needs " << worst << " ms for a 4K frame");
    }
}

TEST_CASE("the presenter's CPU path shows exactly what the pipeline renders") {
    pf::Context context({.headless = true});
    pf::Window window({.title = "t", .width = 240, .height = 160, .scale = 2, .vsync = false});
    REQUIRE_FALSE(window.accelerated());
    video::Options o;
    REQUIRE_FALSE(video::parse_filter("scale2x+scanlines", o));
    video::Presenter presenter(window, o);
    CHECK_FALSE(presenter.gpu());
    const std::vector<std::uint16_t> frame = test_frame(240, 160);
    pf::Image shot;
    presenter.present(frame, 240, 160, &shot);
    const auto [rw, rh] = window.render_size();
    video::Pipeline reference(o);
    const video::Image& expected = reference.render(frame, 240, 160, rw, rh);
    REQUIRE(shot.width == rw);
    REQUIRE(shot.height == rh);
    CHECK(std::equal(shot.rgba.begin(), shot.rgba.end(), reinterpret_cast<const std::uint8_t*>(expected.pixels.data())));

    // A fixed render size is used whatever the window size.
    o.output = video::Output::P720;
    presenter.set_options(o);
    CHECK(window.render_size() == std::pair{1280, 720});
    presenter.present(frame, 240, 160, &shot);
    CHECK(shot.width == 1280);
    CHECK(shot.height == 720);
    CHECK(shot.rgba[0] == 0);  // letterbox
}

TEST_CASE("the GPU path renders 4K in a hidden window within the frame budget") {
    // A real (hidden, never shown) window: the dummy driver used elsewhere has no GPU.
    std::optional<pf::Context> context;
    std::optional<pf::Window> window;
    try {
        context.emplace(pf::ContextOptions{.headless = false});
        window.emplace(pf::WindowConfig{.title = "t", .width = 240, .height = 160, .scale = 1, .vsync = false, .hidden = true});
    } catch (const pf::Error& e) {
        MESSAGE("no display for the GPU test: " << e.what());
        return;
    }
    if (!window->accelerated()) {
        MESSAGE("no GPU renderer; skipping");
        return;
    }
    video::Options o;
    REQUIRE_FALSE(video::apply_serialized(o, "filter=xbrz4x+lcd-grid;color=gba-lcd;output=4k"));
    video::Presenter presenter(*window, o);
    REQUIRE(presenter.gpu());
    REQUIRE(window->render_size() == std::pair{3840, 2160});
    const std::vector<std::uint16_t> frame = test_frame(240, 160);
    pf::Image shot;
    presenter.present(frame, 240, 160, &shot);
    REQUIRE(shot.width == 3840);
    REQUIRE(shot.height == 2160);

    // The GPU's bilinear scaling differs from ours in rounding only.
    video::Pipeline reference(o);
    const video::Image& expected = reference.render(frame, 240, 160, 3840, 2160);
    const auto* want = reinterpret_cast<const std::uint8_t*>(expected.pixels.data());
    double sum = 0;
    for (std::size_t i = 0; i < shot.rgba.size(); ++i) {
        const double d = static_cast<double>(shot.rgba[i]) - want[i];
        sum += d * d;
    }
    const double mse = sum / static_cast<double>(shot.rgba.size());
    MESSAGE("GPU vs CPU 4K MSE " << mse);
    CHECK(mse < 4.0);

    double best = 1e9;
    for (int i = 0; i < 10; ++i) {
        const std::uint64_t t0 = pf::now_ns();
        presenter.present(frame, 240, 160);
        best = std::min(best, static_cast<double>(pf::now_ns() - t0) / 1e6);
    }
    MESSAGE("GPU path: " << best << " ms per 4K frame");
    if (timing_checked()) {
        CHECK(best < 1000.0 / 60.0);
    }
}
