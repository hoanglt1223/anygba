#include "build.h"

#include <doctest.h>

#include <string_view>
#include <vector>

using namespace anygba::driver;

TEST_CASE("anygba build: program names from titles") {
    CHECK(program_name("TONC IRQ") == "TONC IRQ");
    CHECK(program_name("  a/b:c*?  ") == "abc");
    CHECK(program_name("\x01\x02") == "game");
    CHECK(program_name("") == "game");
}

TEST_CASE("anygba build: argument parsing") {
    BuildOptions o;
    const std::vector<std::string_view> args = {"game.gba", "--config", "g.toml", "--miss-log", "a.log", "--miss-log",
                                                "b.log",    "-o",       "dist",   "--name",     "My Game!", "--no-embed",
                                                "--build-type", "Debug"};
    REQUIRE_FALSE(parse_build_args(args, o));
    CHECK(o.rom == "game.gba");
    REQUIRE(o.config);
    CHECK(*o.config == "g.toml");
    CHECK(o.miss_logs.size() == 2);
    CHECK(o.output_dir == "dist");
    REQUIRE(o.name);
    CHECK(*o.name == "My Game");
    CHECK_FALSE(o.embed_rom);
    CHECK(o.build_type == "Debug");

    BuildOptions e;
    CHECK(parse_build_args(std::vector<std::string_view>{}, e) == "no ROM given");
    CHECK(parse_build_args(std::vector<std::string_view>{"a.gba", "--bogus", "x"}, e)->starts_with("unknown option"));
    CHECK(parse_build_args(std::vector<std::string_view>{"a.gba", "--config"}, e).has_value());
    BuildOptions two;
    CHECK(parse_build_args(std::vector<std::string_view>{"a.gba", "b.gba"}, two)->starts_with("unexpected argument"));
}
