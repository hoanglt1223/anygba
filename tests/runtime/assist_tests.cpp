#include "player/assist.h"

#include <doctest.h>

namespace assist = anygba::player::assist;
using assist::Button;
using assist::Edge;

namespace {

constexpr assist::FRect kSafe{0, 0, 2400, 1080};
constexpr std::uint64_t kMs = 1'000'000;

Button placed(Edge edge = Edge::Right, int pos = 500) {
    Button b;
    b.place(edge, pos, kSafe, assist::diameter(kSafe.w, kSafe.h));
    return b;
}

}  // namespace

TEST_CASE("assist: the resting place is against its edge and inside the safe area") {
    const float d = assist::diameter(kSafe.w, kSafe.h);
    const auto right = assist::resting_centre(Edge::Right, 0, kSafe, d);
    CHECK(right.x + d / 2 <= kSafe.w);
    CHECK(right.x > kSafe.w - d * 1.5f);
    CHECK(right.y - d / 2 >= 0);
    const auto bottom = assist::resting_centre(Edge::Bottom, 1000, kSafe, d);
    CHECK(bottom.y + d / 2 <= kSafe.h);
    CHECK(bottom.x + d / 2 <= kSafe.w);
}

TEST_CASE("assist: a release snaps to the nearest edge and keeps its place along it") {
    const float d = assist::diameter(kSafe.w, kSafe.h);
    CHECK(assist::snap(100, 540, kSafe, d).first == Edge::Left);
    CHECK(assist::snap(2350, 300, kSafe, d).first == Edge::Right);
    CHECK(assist::snap(1200, 30, kSafe, d).first == Edge::Top);
    CHECK(assist::snap(1200, 1050, kSafe, d).first == Edge::Bottom);
    const auto [edge, pos] = assist::snap(100, 540, kSafe, d);
    const auto c = assist::resting_centre(edge, pos, kSafe, d);
    CHECK(c.y == doctest::Approx(540).epsilon(0.01));
}

TEST_CASE("assist: a short touch on the button is a tap; elsewhere it is not taken") {
    Button b = placed();
    const auto c = b.centre(0);
    CHECK_FALSE(b.down(1, c.x - 500, c.y, 0));
    REQUIRE(b.down(1, c.x, c.y, 0));
    CHECK(b.tick(100 * kMs) == Button::Gesture::None);
    CHECK(b.up(1, 120 * kMs) == Button::Gesture::Tap);
    CHECK_FALSE(b.held());
}

TEST_CASE("assist: holding still fires one long press and no tap") {
    Button b = placed();
    const auto c = b.centre(0);
    REQUIRE(b.down(7, c.x, c.y, 0));
    CHECK(b.tick(Button::kLongPressNs - 1) == Button::Gesture::None);
    CHECK(b.tick(Button::kLongPressNs) == Button::Gesture::LongPress);
    CHECK(b.tick(Button::kLongPressNs * 2) == Button::Gesture::None);
    CHECK(b.up(7, Button::kLongPressNs * 2) == Button::Gesture::None);
}

TEST_CASE("assist: dragging moves the button and the drop snaps it to the new edge") {
    Button b = placed(Edge::Right, 500);
    const auto c = b.centre(0);
    REQUIRE(b.down(2, c.x, c.y, 0));
    b.move(2, 150, 500, 50 * kMs);
    CHECK(b.centre(50 * kMs).x == doctest::Approx(150));
    CHECK(b.tick(Button::kLongPressNs * 2) == Button::Gesture::None);  // a drag is never a long press
    CHECK(b.up(2, Button::kLongPressNs * 2) == Button::Gesture::Dropped);
    CHECK(b.dropped().first == Edge::Left);
    const auto end = b.centre(Button::kLongPressNs * 2 + Button::kSnapNs);
    CHECK(end.x < b.diameter() * 1.5f);
    CHECK(end.y == doctest::Approx(500).epsilon(0.02));
}

TEST_CASE("assist: the button fades after a while without use") {
    Button b = placed();
    b.wake(0);
    CHECK(b.activity(Button::kIdleNs - 1) == 1.0f);
    CHECK(b.activity(Button::kIdleNs + Button::kFadeNs / 2) == doctest::Approx(0.5f));
    CHECK(b.activity(Button::kIdleNs + Button::kFadeNs) == 0.0f);
}

TEST_CASE("assist: the quick menu grid fits the safe area and finds its cells") {
    for (const bool portrait : {false, true}) {
        const float w = portrait ? 1080.0f : 2400.0f, h = portrait ? 2400.0f : 1080.0f;
        const auto g = assist::grid(assist::kItems, w, h, {0, 0, w, h});
        REQUIRE(g.cells.size() == static_cast<std::size_t>(assist::kItems));
        CHECK(g.card.x >= 0);
        CHECK(g.card.y >= 0);
        CHECK(g.card.x + g.card.w <= w);
        CHECK(g.card.y + g.card.h <= h);
        for (std::size_t i = 0; i < g.cells.size(); ++i) {
            const auto& c = g.cells[i];
            CHECK(assist::cell_at(g, c.x + c.w / 2, c.y + c.h / 2) == static_cast<int>(i));
        }
        CHECK(assist::cell_at(g, 1, 1) == -1);
    }
}

TEST_CASE("assist: toolbar buttons are hit where they are drawn") {
    const auto bar = assist::toolbar(2400, 1080, kSafe);
    for (int i = 0; i < assist::kTools; ++i) {
        const auto& b = bar[static_cast<std::size_t>(i)];
        CHECK(assist::tool_at(bar, b.x + b.w / 2, b.y + b.h / 2) == static_cast<assist::Tool>(i));
    }
    CHECK_FALSE(assist::tool_at(bar, 1, 1).has_value());
}
