#include "config/game_config.h"

#include <doctest.h>

#include <string>
#include <vector>

using namespace anygba::config;
using anygba::analysis::Mode;

namespace {

constexpr const char* kSample = R"(
[game]
title = "sample"

[[entry]]
addr = 0x08000100
mode = "thumb"

[[ram_region]]
rom_src = 0x08000200
ram_dst = 0x03000000
size = 0x100
mode = "arm"

[[skip]]
start = 0x08000300
end = 0x08000400

[[patch]]
addr = 0x08000010
bytes = "00 00 a0 e1"
)";

std::string error_of(const char* text) {
    try {
        parse_game_config(text, "game.toml");
    } catch (const ConfigError& e) {
        return e.what();
    }
    return "";
}

}  // namespace

TEST_CASE("game config parses every section") {
    const GameConfig c = parse_game_config(kSample, "game.toml");
    CHECK(c.title == "sample");
    REQUIRE(c.entries.size() == 1);
    CHECK(c.entries[0].addr == 0x08000100);
    CHECK(c.entries[0].mode == Mode::Thumb);
    REQUIRE(c.ram_code.size() == 1);
    CHECK(c.ram_code[0].region.ram_dst == 0x03000000);
    REQUIRE(c.skip.size() == 1);
    CHECK(c.skip[0].end == 0x08000400);
    REQUIRE(c.patches.size() == 1);
    CHECK(c.patches[0].bytes == std::vector<std::uint8_t>{0x00, 0x00, 0xA0, 0xE1});

    anygba::analysis::Image image(std::vector<std::uint8_t>(0x1000, 0xFF));
    const auto options = apply_game_config(c, image);
    CHECK(image.read32(0x08000010) == 0xE1A00000);
    CHECK(image.read32(0x03000000) == 0xFFFFFFFF);
    REQUIRE(options.entries.size() == 2);
    CHECK(options.entries[0].addr == 0x03000000);
    CHECK(options.skip.size() == 1);
}

TEST_CASE("game config errors name the file, line, and key") {
    CHECK(error_of("[[entry]]\naddr = 0x100\nmode = \"arm\"\nmood = 1\n") == "game.toml:4: unknown key 'mood' in [[entry]]");
    CHECK(error_of("[[entry]]\naddr = 0x100\nmode = \"x86\"\n") == "game.toml:3: entry.mode must be \"arm\" or \"thumb\"");
    CHECK(error_of("[[entry]]\nmode = \"arm\"\n").starts_with("game.toml:1: entry needs 'addr'"));
    CHECK(error_of("[[entry]]\naddr = -1\nmode = \"arm\"\n").starts_with("game.toml:2: entry.addr must be"));
    CHECK(error_of("[[skip]]\nstart = 4\nend = 4\n").starts_with("game.toml:1: skip.end must be greater"));
    CHECK(error_of("[[patch]]\naddr = 4\nbytes = \"abc\"\n").starts_with("game.toml:3: patch.bytes must be"));
    CHECK(error_of("bogus = 1\n") == "game.toml:1: unknown key 'bogus' in the top level");
    CHECK(error_of("entry = 1\n").starts_with("game.toml:1: 'entry' must be an array of tables"));
    CHECK(error_of("[[entry]\n").starts_with("game.toml:1:"));
}

TEST_CASE("game config rejects regions and patches outside the image") {
    anygba::analysis::Image image(std::vector<std::uint8_t>(0x100, 0));
    GameConfig bad_region;
    bad_region.ram_code.push_back({{0x08000000, 0x05000000, 0x10}, Mode::Arm});
    CHECK_THROWS_AS(apply_game_config(bad_region, image), ConfigError);
    GameConfig bad_patch;
    bad_patch.patches.push_back({0x08001000, {1, 2}});
    CHECK_THROWS_AS(apply_game_config(bad_patch, image), ConfigError);
}
