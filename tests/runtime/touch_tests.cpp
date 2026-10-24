#include "player/touch.h"

#include <doctest.h>

namespace touch = anygba::player::touch;
using touch::Control;

namespace {

constexpr std::uint16_t kA = 1u << 0, kB = 1u << 1, kStart = 1u << 3, kRight = 1u << 4, kLeft = 1u << 5,
                        kUp = 1u << 6, kDown = 1u << 7, kL = 1u << 9;

float cx(const touch::FRect& r) { return r.x + r.w / 2; }
float cy(const touch::FRect& r) { return r.y + r.h / 2; }

bool overlaps(const touch::FRect& a, const touch::FRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

bool within(const touch::FRect& inner, const touch::FRect& outer) {
    return inner.x >= outer.x - 0.01f && inner.y >= outer.y - 0.01f && inner.x + inner.w <= outer.x + outer.w + 0.01f &&
           inner.y + inner.h <= outer.y + outer.h + 0.01f;
}

}  // namespace

TEST_CASE("touch: landscape puts the picture at a whole scale in the middle of the safe area") {
    const touch::FRect safe{80, 0, 2240, 1080};  // a notch on the left
    const auto l = touch::layout(2400, 1080, safe, {});
    CHECK(l.picture.w == 1440);
    CHECK(l.picture.h == 960);
    CHECK(cx(l.picture) == doctest::Approx(cx(safe)));
    CHECK(cy(l.picture) == doctest::Approx(540));
    for (const auto& c : l.controls) CHECK(within(c, safe));
}

TEST_CASE("touch: portrait puts the picture at the top and every control below it") {
    const touch::FRect safe{0, 100, 1080, 2200};
    const auto l = touch::layout(1080, 2400, safe, {});
    CHECK(l.picture.y == 100);
    CHECK(l.picture.w == 960);
    for (const auto& c : l.controls) {
        CHECK(within(c, safe));
        CHECK_FALSE(overlaps(c, l.picture));
    }
}

TEST_CASE("touch: small outputs scale the picture down to fit") {
    const auto l = touch::layout(200, 100, {0, 0, 200, 100}, {});
    CHECK(l.picture.w <= 200);
    CHECK(l.picture.h <= 100);
}

TEST_CASE("touch: controls do not overlap each other") {
    for (const bool portrait : {false, true}) {
        const float w = portrait ? 1080.0f : 2400.0f, h = portrait ? 2400.0f : 1080.0f;
        for (const int size : {50, 100, 200}) {
            touch::Settings s;
            s.size = size;
            const auto l = touch::layout(w, h, {0, 0, w, h}, s);
            for (int i = 0; i < touch::kControls; ++i) {
                for (int j = i + 1; j < touch::kControls; ++j) {
                    INFO("portrait " << portrait << " size " << size << " controls " << i << " " << j);
                    CHECK_FALSE(overlaps(l.controls[static_cast<std::size_t>(i)], l.controls[static_cast<std::size_t>(j)]));
                }
            }
        }
    }
}

TEST_CASE("touch: the d-pad has eight directions and a dead centre") {
    const auto l = touch::layout(2400, 1080, {0, 0, 2400, 1080}, {});
    const auto& d = l[Control::DPad];
    const float r = d.w / 2;
    CHECK(touch::buttons_at(l, cx(d), cy(d)) == 0);
    CHECK(touch::buttons_at(l, cx(d) + r * 0.8f, cy(d)) == kRight);
    CHECK(touch::buttons_at(l, cx(d) - r * 0.8f, cy(d)) == kLeft);
    CHECK(touch::buttons_at(l, cx(d), cy(d) - r * 0.8f) == kUp);
    CHECK(touch::buttons_at(l, cx(d), cy(d) + r * 0.8f) == kDown);
    CHECK(touch::buttons_at(l, cx(d) + r * 0.5f, cy(d) - r * 0.5f) == (kUp | kRight));
    CHECK(touch::buttons_at(l, cx(d) - r * 0.5f, cy(d) + r * 0.5f) == (kDown | kLeft));
    CHECK(touch::buttons_at(l, cx(d) + r * 1.1f, cy(d)) == kRight);  // slid a little past the edge
}

TEST_CASE("touch: buttons map to GBA keys and player functions") {
    touch::Settings classic;
    classic.floating_button = false;
    const auto l = touch::layout(2400, 1080, {0, 0, 2400, 1080}, classic);
    const auto at = [&l](Control c) { return touch::buttons_at(l, cx(l[c]), cy(l[c])); };
    CHECK(at(Control::A) == kA);
    CHECK(at(Control::B) == kB);
    CHECK(at(Control::L) == kL);
    CHECK(at(Control::Start) == kStart);
    CHECK(at(Control::Menu) == touch::kMenu);
    CHECK(at(Control::FastForward) == touch::kFastForward);
    CHECK(touch::buttons_at(l, 1200, 540) == 0);  // the middle of the picture
}

TEST_CASE("touch: offsets move the controls in from the edges") {
    touch::Settings moved;
    moved.offset_x = 50;
    moved.offset_y = 50;
    const auto a = touch::layout(2400, 1080, {0, 0, 2400, 1080}, {});
    const auto b = touch::layout(2400, 1080, {0, 0, 2400, 1080}, moved);
    CHECK(b[Control::DPad].x > a[Control::DPad].x);
    CHECK(b[Control::DPad].y < a[Control::DPad].y);
    CHECK(b[Control::A].x < a[Control::A].x);
}

TEST_CASE("touch: settings round-trip, clamp and skip bad lines") {
    touch::Settings s;
    s.size = 150;
    s.opacity = 70;
    s.offset_x = -20;
    s.offset_y = 30;
    s.haptics = false;
    CHECK(touch::parse_settings(touch::format_settings(s)) == s);

    const auto t = touch::parse_settings("size = 999\nOpacity=abc\r\nOffsetX = -500 ; far\nunknown = 3\nnoequals\n");
    CHECK(t.size == 200);
    CHECK(t.opacity == touch::Settings{}.opacity);
    CHECK(t.offset_x == -100);
    CHECK(touch::parse_settings("") == touch::Settings{});
}

TEST_CASE("touch: floating button, hidden controls and per-control adjustments round-trip") {
    touch::Settings s;
    s.floating_button = false;
    s.show_controls = false;
    s.button_edge = touch::Edge::Bottom;
    s.button_pos = 750;
    s.adjust[static_cast<std::size_t>(Control::A)] = {120, -40, 150};
    s.adjust[static_cast<std::size_t>(Control::DPad)] = {-30, 0, 80};
    CHECK(touch::parse_settings(touch::format_settings(s)) == s);

    const auto t = touch::parse_settings("ASize = 999\nDPadX = -5000\nButtonEdge = 9\nButtonPos = -3\nBogusX = 1\n");
    CHECK(t.adjust[static_cast<std::size_t>(Control::A)].size == 200);
    CHECK(t.adjust[static_cast<std::size_t>(Control::DPad)].x == -1000);
    CHECK(t.button_edge == touch::Edge::Bottom);
    CHECK(t.button_pos == 0);
}

TEST_CASE("touch: an adjusted control moves and grows, and stays inside the safe area") {
    const touch::FRect safe{0, 0, 2400, 1080};
    const auto base = touch::layout(2400, 1080, safe, {});
    touch::Settings s;
    s.adjust[static_cast<std::size_t>(Control::A)] = {-100, -100, 150};
    const auto l = touch::layout(2400, 1080, safe, s);
    const auto& a0 = base[Control::A];
    const auto& a1 = l[Control::A];
    CHECK(a1.w == doctest::Approx(a0.w * 1.5f));
    CHECK(cx(a1) == doctest::Approx(cx(a0) - l.unit));
    CHECK(cy(a1) == doctest::Approx(cy(a0) - l.unit));
    CHECK(touch::buttons_at(l, cx(a1), cy(a1)) == kA);

    s.adjust[static_cast<std::size_t>(Control::A)] = {1000, 1000, 200};
    CHECK(within(touch::layout(2400, 1080, safe, s)[Control::A], safe));
}

TEST_CASE("touch: MENU only works without the floating button; hidden controls press nothing") {
    const touch::FRect safe{0, 0, 2400, 1080};
    touch::Settings s;
    const auto floating = touch::layout(2400, 1080, safe, s);
    const auto& m = floating[Control::Menu];
    CHECK(touch::buttons_at(floating, cx(m), cy(m)) == 0);
    CHECK(touch::control_at(floating, cx(m), cy(m)) != Control::Menu);

    s.floating_button = false;
    const auto classic = touch::layout(2400, 1080, safe, s);
    CHECK(touch::buttons_at(classic, cx(m), cy(m)) == touch::kMenu);
    CHECK(touch::control_at(classic, cx(m), cy(m)) == Control::Menu);

    s.show_controls = false;
    const auto hidden = touch::layout(2400, 1080, safe, s);
    CHECK(touch::buttons_at(hidden, cx(hidden[Control::A]), cy(hidden[Control::A])) == 0);
    CHECK(touch::buttons_at(hidden, cx(m), cy(m)) == touch::kMenu);
    CHECK(touch::control_at(hidden, cx(hidden[Control::A]), cy(hidden[Control::A])) == Control::A);
}

TEST_CASE("touch: a widescreen picture keeps its aspect ratio") {
    const auto l = touch::layout(2400, 1080, {0, 0, 2400, 1080}, {}, 284);
    CHECK(l.picture.w / l.picture.h == doctest::Approx(284.0f / 160.0f));
    CHECK(l.picture.w <= 2400);
}
