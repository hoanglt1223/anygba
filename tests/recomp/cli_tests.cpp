#include "cli.h"
#include "support/temp_dir.h"

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

using namespace anygba::recomp;

namespace {

ParseResult parse(std::vector<std::string_view> args) {
    return parse_args(args);
}

}  // namespace

TEST_CASE("recomp CLI parses a ROM with options") {
    const auto result = parse({"game.gba", "--config", "game.toml", "-o", "out"});
    REQUIRE(result.action == Action::Run);
    CHECK(result.options.rom == "game.gba");
    REQUIRE(result.options.config);
    CHECK(*result.options.config == "game.toml");
    CHECK(result.options.output_dir == "out");
}

TEST_CASE("recomp CLI defaults") {
    const auto result = parse({"game.gba"});
    REQUIRE(result.action == Action::Run);
    CHECK_FALSE(result.options.config);
    CHECK(result.options.output_dir == "recomp-out");
}

TEST_CASE("recomp CLI help and version win over other arguments") {
    CHECK(parse({"--help"}).action == Action::Help);
    CHECK(parse({"game.gba", "-h"}).action == Action::Help);
    CHECK(parse({"--version"}).action == Action::Version);
    CHECK(version().starts_with("anygba-recomp "));
    CHECK(usage().find("--config") != std::string::npos);
}

TEST_CASE("recomp CLI errors") {
    CHECK(parse({}).error == "missing ROM path");
    CHECK(parse({"--config"}).error == "--config needs a value");
    CHECK(parse({"game.gba", "--output", ""}).error == "--output needs a value");
    CHECK(parse({"--bogus", "game.gba"}).error == "unknown option: --bogus");
    CHECK(parse({"a.gba", "b.gba"}).action == Action::Error);
}

TEST_CASE("recomp CLI input validation") {
    const anygba::test::TempDir temp("anygba-cli-test");
    const auto& dir = temp.path;
    const auto rom = dir / "fake.gba";
    std::ofstream(rom) << "not a rom";

    Options options;
    options.rom = rom;
    CHECK_FALSE(validate_inputs(options));

    options.config = dir / "missing.toml";
    const auto error = validate_inputs(options);
    REQUIRE(error);
    CHECK(error->starts_with("config not found"));

    options.rom = dir / "missing.gba";
    options.config.reset();
    CHECK(validate_inputs(options)->starts_with("ROM not found"));

    options.rom = dir;
    CHECK(validate_inputs(options)->starts_with("ROM not found"));
}
