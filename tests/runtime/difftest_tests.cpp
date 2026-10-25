#include "overrides/difftest.h"

#include "anygba/override.hpp"
#include "anygba/system.hpp"
#include "control/json.h"
#include "overrides/overrides.h"

#include <doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace dt = anygba::difftest;
using anygba::control::Json;

constexpr std::uint32_t kFill = 0x08000100;  // ARM; its code is never interpreted here

std::uint32_t g_hits[3];
const std::uint32_t g_blocks[3] = {kFill, kFill + 4, kFill + 8};
std::uint8_t g_on[1];

// The "translation": clears r1 words at r0, charging 3 instructions per word.
void fill_original(anygba_cpu* c) {
    ++g_hits[0];
    while (c->r[1] != 0) {
        ++g_hits[1];
        ag_wr32(c, c->r[0], 0);
        c->r[0] += 4;
        c->r[1] -= 1;
        c->cycles -= 3 * c->fetch_cost[c->t][8];
        if (c->cycles <= 0) {
            c->r[15] = kFill + 4;
            anygba_tick(c);
        }
    }
    ++g_hits[2];
    ag_return(c);
}

void fill_wrapper(anygba_cpu* c) {
    if (g_on[0]) {
        anygba_override_run(c, 0);
        return;
    }
    fill_original(c);
}

ANYGBA_OVERRIDE(difftest_unit_fill, g) {
    std::uint32_t dest = g.reg(0);
    while (g.reg(1) != 0) {
        g.write<std::uint32_t>(dest, 0);
        dest += 4;
        g.reg(0) = dest;
        g.reg(1) -= 1;
        g.block(kFill + 4, 3);
    }
    g.ret();
}

ANYGBA_OVERRIDE(difftest_unit_fill_off_by_one, g) {
    std::uint32_t dest = g.reg(0);
    for (std::uint32_t i = 0; i <= g.reg(1); ++i) {
        g.write<std::uint32_t>(dest, 0);
        dest += 4;
    }
    g.reg(0) = dest;
    g.reg(1) = 0;
    g.ret();
}

const anygba_function_entry kFunctions[] = {{kFill, 0, fill_wrapper, 0, 0, 0}};
const anygba_override_entry kEntry{kFill, 0, "difftest_unit_fill", "Fill", "", 1, fill_original, 0, g_hits, g_blocks, 3};

anygba_game test_game() {
    anygba_game game{};
    game.title = "difftest test";
    game.functions = kFunctions;
    game.function_count = 1;
    game.save_type = "none";
    game.rtc = 0;
    game.overrides = &kEntry;
    game.override_count = 1;
    game.override_on = g_on;
    return game;
}

const char* const kSpec = R"(
["0x08000100"]
dead_stack = 0

[["0x08000100".shape]]
name = "words"
r0 = { pointer = "ewram", align = 4 }
r1 = { range = [0, 64] }
)";

std::filesystem::path scratch(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "anygba-difftest-tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

Json read_json(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
}

anygba::Options quiet() {
    anygba::Options o;
    o.log_misses = false;
    return o;
}

std::vector<std::uint8_t> test_rom() { return std::vector<std::uint8_t>(0x400, 0); }

}  // namespace

TEST_CASE("difftest: spec parsing, defaults and errors with file and line") {
    const auto spec = dt::parse_spec(R"(
[StringCopy]
dead_stack = 64
ignore_memory = [[0x03007f00, 0x03008000]]
[[StringCopy.shape]]
name = "ram-src"
weight = 3
r0 = "ram"
r1 = { string = ["ewram", "iwram"], length = [0, 10] }
r2 = { near = "r1", offset = [-8, 0] }
cycles = { range = [0, 400] }
)",
                                     "spec.toml", "StringCopy", 0x08008BA0);
    REQUIRE(spec);
    CHECK(spec->dead_stack == 64);
    REQUIRE(spec->ignore_memory.size() == 1);
    REQUIRE(spec->shapes.size() == 1);
    const dt::Shape& s = spec->shapes[0];
    CHECK(s.name == "ram-src");
    CHECK(s.weight == 3);
    CHECK(s.values[0].kind == dt::Value::Kind::Pointer);
    CHECK(s.values[0].regions.size() == 2);
    CHECK(s.values[1].kind == dt::Value::Kind::String);
    CHECK(s.values[1].max_len == 10);
    CHECK(s.values[2].kind == dt::Value::Kind::Near);
    CHECK(s.values[3].kind == dt::Value::Kind::Any);
    CHECK(s.values[13].kind == dt::Value::Kind::Range);  // default sp
    CHECK(s.values[14].thumb);                            // default lr: Thumb code in ROM
    CHECK(s.values[dt::kCyclesSlot].kind == dt::Value::Kind::Range);

    CHECK_FALSE(dt::parse_spec("[Other]\n[[Other.shape]]\n", "spec.toml", "StringCopy", 0x08008BA0));
    CHECK(dt::parse_spec("[\"0x08008ba0\"]\n[[\"0x08008ba0\".shape]]\n", "spec.toml", "StringCopy", 0x08008BA0));

    CHECK_THROWS_WITH_AS(dt::parse_spec("[F]\n[[F.shape]]\nr0 = { strnig = \"ram\" }\n", "s.toml", "F", 0),
                         doctest::Contains("s.toml:3: r0: unknown key 'strnig'"), anygba::Error);
    CHECK_THROWS_WITH_AS(
        dt::parse_spec("[F]\n[[F.shape]]\nr1 = { near = \"r2\" }\nr2 = { near = \"sp\" }\n", "s.toml", "F", 0),
        doctest::Contains("r1.near: r2 is itself near another register"), anygba::Error);
    CHECK_THROWS_WITH_AS(dt::parse_spec("[F]\n[[F.shape]]\nr1 = { string = \"rom\" }\n", "s.toml", "F", 0),
                         doctest::Contains("not in rom"), anygba::Error);
    CHECK_THROWS_WITH_AS(dt::parse_spec("[F]\ndead_stack = 1\n", "s.toml", "F", 0),
                         doctest::Contains("needs at least one [[F.shape]]"), anygba::Error);
}

TEST_CASE("difftest: random states are deterministic, planted strings end with the terminator, forcing one slot keeps the rest") {
    const auto spec = dt::parse_spec(R"(
[F]
[[F.shape]]
r0 = { pointer = "iwram", align = 4 }
r1 = { string = "ewram", length = [5, 20], terminator = 0xFF }
r3 = { range = [10, 20] }
r2 = { near = "r3", offset = [-2, -2] }
)",
                                     "spec.toml", "F", 0);
    REQUIRE(spec);
    const dt::Generated a = dt::generate(*spec, 0, 7, 3, {}, 0x400);
    const dt::Generated b = dt::generate(*spec, 0, 7, 3, {}, 0x400);
    CHECK(a.state.r == b.state.r);
    CHECK(a.state.r[0] % 4 == 0);
    CHECK((a.state.r[0] >> 24) == 0x03);
    CHECK(a.state.r[3] >= 10);
    CHECK(a.state.r[3] <= 20);
    CHECK(a.state.r[2] == a.state.r[3] - 2);  // near a register chosen after it in r0-r12 order
    REQUIRE(a.state.pokes.size() == 1);
    const dt::Poke& p = a.state.pokes[0];
    CHECK(p.addr == a.state.r[1]);
    CHECK(p.bytes.size() == *a.params[1] + 1);
    CHECK(p.bytes.back() == 0xFF);
    for (std::size_t i = 0; i + 1 < p.bytes.size(); ++i) CHECK(p.bytes[i] != 0xFF);

    dt::Params forced;
    forced[1] = 5;
    const dt::Generated c = dt::generate(*spec, 0, 7, 3, forced, 0x400);
    CHECK(c.state.pokes[0].bytes.size() == 6);
    CHECK(c.state.r[0] == a.state.r[0]);
    CHECK(c.state.r[3] == a.state.r[3]);
    CHECK(c.state.r[1] == a.state.r[1]);  // same place, shorter string
}

TEST_CASE("difftest: recorded states survive a round trip; damaged files are refused") {
    dt::Recording rec;
    rec.entry = 0x08008BA0;
    rec.thumb = 1;
    rec.base = {1, 2, 3};
    dt::State s;
    s.r[0] = 0x02000010;
    s.flags = 0x60000000;
    s.cycles = 77;
    s.pokes.push_back({0x02000010, {0x41, 0xFF}});
    rec.states.push_back(s);
    const auto path = scratch("roundtrip.states");
    dt::write_recording(path, rec);
    const dt::Recording back = dt::read_recording(path);
    CHECK(back.entry == rec.entry);
    CHECK(back.base == rec.base);
    REQUIRE(back.states.size() == 1);
    CHECK(back.states[0].r == s.r);
    CHECK(back.states[0].cycles == 77);
    CHECK(back.states[0].pokes[0].bytes == s.pokes[0].bytes);

    write(path, "AGDTREC1\x01");
    CHECK_THROWS_WITH_AS(dt::read_recording(path), doctest::Contains("truncated"), anygba::Error);
    write(path, "something else");
    CHECK_THROWS_WITH_AS(dt::read_recording(path), doctest::Contains("not a recorded states file"), anygba::Error);
}

TEST_CASE("difftest: a correct override passes with full coverage; an off-by-one one fails with a minimal diff") {
    const anygba_game game = test_game();
    anygba::overrides::activate(game, anygba::overrides::Mode::On);
    const auto spec = scratch("fill.toml");
    write(spec, kSpec);

    {
        anygba::System sys(test_rom(), &game, quiet());
        dt::Options o;
        o.function = "Fill";
        o.spec = spec;
        o.states = 300;
        o.json = scratch("pass.json");
        CHECK(dt::run(sys, game, o) == 0);
        const Json j = read_json(*o.json);
        CHECK(j.find("pass")->as_bool());
        CHECK((*j.find("states")).find("total")->as_int() == 300);
        CHECK((*j.find("states")).find("full_identical")->as_int() == 300);
        CHECK((*j.find("coverage")).find("covered")->as_int() == 3);
        CHECK(j.find("first_failure")->is_null());
        CHECK(g_on[0] == 1);  // left as activated
    }
    {
        anygba::System sys(test_rom(), &game, quiet());
        dt::Options o;
        o.function = "0x08000100";
        o.impl = "difftest_unit_fill_off_by_one";
        o.spec = spec;
        o.states = 300;
        o.json = scratch("fail.json");
        CHECK(dt::run(sys, game, o) == 1);
        const Json j = read_json(*o.json);
        CHECK_FALSE(j.find("pass")->as_bool());
        const Json& f = *j.find("first_failure");
        CHECK((*f.find("minimised_input")).find("r")->as_array()[1].as_string() == "0x00000000");
        const std::string diffs = f.find("minimised_differences")->dump();
        CHECK(diffs.find("original 0 writes, override 1") != std::string::npos);
        CHECK(diffs.find("\"r0\"") != std::string::npos);
    }
    {
        anygba::System sys(test_rom(), &game, quiet());
        dt::Options o;
        o.function = "Nope";
        CHECK_THROWS_WITH_AS(dt::run(sys, game, o), doctest::Contains("no override named Nope"), anygba::Error);
        o.function = "Fill";
        CHECK_THROWS_WITH_AS(dt::run(sys, game, o), doctest::Contains("nothing to test"), anygba::Error);
    }
    anygba::overrides::set_impl(0, anygba::overrides::find_impl("difftest_unit_fill"));
    anygba::overrides::activate(game, anygba::overrides::Mode::Off);
}
