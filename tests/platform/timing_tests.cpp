#include <anyport/platform/context.h>
#include <anyport/platform/timing.h>

#include <doctest.h>

#include <limits>

namespace pf = anyport::platform;

TEST_CASE("frame pacer validates the rate") {
    CHECK_THROWS_AS((pf::FramePacer{0.0}), pf::Error);
    CHECK_THROWS_AS((pf::FramePacer{-60.0}), pf::Error);
    CHECK_THROWS_AS((pf::FramePacer{std::numeric_limits<double>::infinity()}), pf::Error);
    CHECK_THROWS_AS((pf::FramePacer{std::numeric_limits<double>::quiet_NaN()}), pf::Error);
    CHECK(pf::FramePacer(59.7275).period_ns() == 16742706);
}

TEST_CASE("frame pacer never returns early") {
    pf::Context context({.headless = true});
    pf::FramePacer pacer(200.0);
    pacer.wait();
    const auto start = pf::now_ns();
    for (int i = 0; i < 10; ++i) {
        pacer.wait();
    }
    // Lower bound only: CI machines may oversleep but must never undersleep.
    CHECK(pf::now_ns() - start >= 10 * pacer.period_ns() - 1'000'000);
}
