#include "codegen/c_emitter.h"
#include "config/game_config.h"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
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

TEST_CASE("game config: save type, replacement functions, and their sources") {
    const GameConfig c = parse_game_config(
        "[game]\nsave = \"flash128\"\nsources = [\"hacks.c\"]\n\n"
        "[[replace]]\naddr = 0x08000000\nmode = \"arm\"\nfunction = \"skip_intro\"\n",
        "game.toml");
    CHECK(c.save_type == "flash128");
    REQUIRE(c.sources.size() == 1);
    CHECK(c.sources[0] == "hacks.c");
    REQUIRE(c.replacements.size() == 1);
    CHECK(c.replacements[0].function == "skip_intro");

    anygba::analysis::Image image(std::vector<std::uint8_t>(0x100, 0));
    const auto options = apply_game_config(c, image);
    REQUIRE(options.entries.size() == 1);
    CHECK(options.entries[0].addr == 0x08000000);

    CHECK(error_of("[game]\nsave = \"flash\"\n").starts_with("game.toml:2: game.save must be one of"));
    CHECK(error_of("[game]\nsources = [\"a.cpp\"]\n").starts_with("game.toml:2: game.sources entries must be .c"));
    CHECK(error_of("[game]\nsources = [\"a.c\"]\n[[replace]]\naddr = 0\nmode = \"arm\"\nfunction = \"f_0800\"\n")
              .starts_with("game.toml:6: replace.function must be a C identifier"));
    CHECK(error_of("[[replace]]\naddr = 0\nmode = \"arm\"\nfunction = \"fix\"\n")
              .find("needs game.sources") != std::string::npos);
}

TEST_CASE("generated project: replaced functions forward to the hand-written C, save type and sources") {
    const auto dir = std::filesystem::temp_directory_path() / "anygba-replace-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto hacks = dir / "hacks.c";
    std::ofstream(hacks, std::ios::binary) << "/* user code */\n";

    std::vector<std::uint8_t> rom(0x100, 0);
    const std::uint8_t bx_lr[] = {0x1E, 0xFF, 0x2F, 0xE1};
    std::copy(std::begin(bx_lr), std::end(bx_lr), rom.begin());
    const anygba::analysis::Image image(rom);
    anygba::analysis::DiscoveryOptions options;
    options.entries.push_back({0x08000000, Mode::Arm, anygba::analysis::EntrySource::RomEntry});
    const auto discovery = anygba::analysis::discover(image, options);

    anygba::codegen::ProjectInfo info;
    info.title = "t";
    info.rom_sha256 = "00";
    info.rom_size = 0x100;
    info.save_type = "eeprom8k";
    info.replacements[0x08000000] = "skip_intro";
    info.sources.push_back(hacks);
    const auto out = dir / "gen";
    anygba::codegen::write_project(out, image, discovery, info);
    const auto read = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    };
    CHECK(read(out / "game.h").find("void skip_intro(anygba_cpu* c);") != std::string::npos);
    CHECK(read(out / "funcs_000.c").find("void f_08000000(anygba_cpu* c) { skip_intro(c); }") != std::string::npos);
    CHECK(read(out / "dispatch.c").find("\"eeprom8k\",") != std::string::npos);
    CHECK(read(out / "hacks.c") == "/* user code */\n");

    info.replacements[0x08000040] = "nowhere";
    CHECK_THROWS_WITH(anygba::codegen::write_project(out, image, discovery, info), doctest::Contains("0x08000040"));
    std::filesystem::remove_all(dir);
}
