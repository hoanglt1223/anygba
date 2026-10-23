#include <anyport/platform/context.h>
#include <anyport/platform/window.h>

#include <doctest.h>

#include <cstdint>
#include <vector>

namespace pf = anyport::platform;

TEST_CASE("window rejects invalid configuration") {
    pf::Context context({.headless = true});
    CHECK_THROWS_AS((pf::Window{{.title = "t", .width = 0, .height = 10}}), pf::Error);
    CHECK_THROWS_AS((pf::Window{{.title = "t", .width = 10, .height = 10, .scale = 0}}), pf::Error);
}

TEST_CASE("present validates the framebuffer size") {
    pf::Context context({.headless = true});
    pf::Window window({.title = "t", .width = 8, .height = 4, .scale = 1, .vsync = false});
    std::vector<std::uint16_t> too_small(8 * 4 - 1);
    CHECK_THROWS_AS(window.present(too_small), pf::Error);
}

TEST_CASE("RGB555 framebuffer is shown with nearest-neighbour integer scaling") {
    pf::Context context({.headless = true});
    constexpr int kScale = 3;
    pf::Window window({.title = "t", .width = 4, .height = 2, .scale = kScale, .vsync = false});

    // Red in the low bits, blue in the high bits.
    const std::vector<std::uint16_t> fb = {
        0x001F, 0x03E0, 0x7C00, 0x7FFF,
        0x0000, 0x7FFF, 0x001F, 0x7C00,
    };
    pf::Image image;
    window.present(fb, &image);

    REQUIRE(image.width == 4 * kScale);
    REQUIRE(image.height == 2 * kScale);
    auto pixel = [&](int x, int y) {
        const auto* p = &image.rgba[static_cast<std::size_t>((y * image.width + x) * 4)];
        return std::vector<int>{p[0], p[1], p[2]};
    };
    for (int dy = 0; dy < kScale; ++dy) {
        for (int dx = 0; dx < kScale; ++dx) {
            CHECK(pixel(0 * kScale + dx, dy) == std::vector<int>{255, 0, 0});
            CHECK(pixel(1 * kScale + dx, dy) == std::vector<int>{0, 255, 0});
            CHECK(pixel(2 * kScale + dx, dy) == std::vector<int>{0, 0, 255});
            CHECK(pixel(3 * kScale + dx, kScale + dy) == std::vector<int>{0, 0, 255});
            CHECK(pixel(0 * kScale + dx, kScale + dy) == std::vector<int>{0, 0, 0});
        }
    }
}
