#include "anygba/override.hpp"
#include "anygba/system.hpp"
#include "overrides/overrides.h"

#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kOverridden = 0x08000100;  // ARM
constexpr std::uint32_t kAddOne = 0x08000200;      // ARM: add r0, r0, #1 ; bx lr
constexpr std::uint32_t kCallSite = 0x08000300;

int g_calls = 0;
bool g_from_recompiled = false;

ANYGBA_OVERRIDE(test_add_two, g) {
    ++g_calls;
    g_from_recompiled = g.from_recompiled_code();
    const std::uint32_t lr = g.reg(14);  // call() sets lr, as bl does
    g.block(kOverridden + 4, 2);
    if (!g.call(kAddOne, kOverridden + 8)) return;
    if (!g.call(kAddOne, kOverridden + 8)) return;
    g.write<std::uint16_t>(0x02000001, 0xBEEF);  // aligned down, like strh
    g.ret_to(lr);
}

ANYGBA_OVERRIDE(test_forgets_to_return, g) { g.set_result(7); }

ANYGBA_OVERRIDE(test_wraps_original, g) {
    ++g_calls;
    g.run_original();
}

std::vector<std::uint8_t> test_rom() {
    std::vector<std::uint8_t> rom(0x400, 0);
    const auto put = [&rom](std::uint32_t off, std::uint32_t w) {
        for (int b = 0; b < 4; ++b) rom.at(off + static_cast<std::uint32_t>(b)) = static_cast<std::uint8_t>(w >> (8 * b));
    };
    put(0x000, 0xEB00003E);  // bl 0x08000100
    put(0x004, 0xEAFFFFFE);  // b .
    put(0x100, 0xE12FFF1E);  // bx lr (the overridden function's own code is never interpreted here)
    put(0x200, 0xE2800001);  // add r0, r0, #1
    put(0x204, 0xE12FFF1E);  // bx lr
    return rom;
}

std::uint8_t g_on[1];

void original(anygba_cpu* c) {
    c->r[0] = 99;
    ag_return(c);
}

// What anygba-recomp generates for an overridden function.
void wrapper(anygba_cpu* c) {
    if (g_on[0]) {
        anygba_override_run(c, 0);
        return;
    }
    original(c);
}

const anygba_function_entry kFunctions[] = {{kOverridden, 0, wrapper, 0, 0, 0}};

anygba_game game_with(const anygba_override_entry* entry) {
    anygba_game game{};
    game.title = "override test";
    game.functions = kFunctions;
    game.function_count = 1;
    game.save_type = "none";
    game.rtc = 0;
    game.overrides = entry;
    game.override_count = 1;
    game.override_on = g_on;
    return game;
}

anygba::Options quiet() {
    anygba::Options o;
    o.log_misses = false;
    return o;
}

// A call from a generated call site: lr and the return stack name kCallSite.
void call_from_recompiled_code(anygba_cpu& c) {
    c.r[14] = kCallSite;
    c.t = 0;
    c.ret_stack[c.ret_depth++] = kCallSite;
    wrapper(&c);
    c.ret_depth--;
}

}  // namespace

TEST_CASE("overrides: activation checks every implementation and follows on/off/all") {
    using anygba::overrides::Mode;
    const anygba_override_entry entry{kOverridden, 0, "test_add_two", "AddTwo", "test", 0, original};
    const anygba_game game = game_with(&entry);

    auto active = anygba::overrides::activate(game, Mode::On);
    REQUIRE(active.size() == 1);
    CHECK_FALSE(active[0].on);  // disabled in the config
    CHECK(g_on[0] == 0);
    CHECK(anygba::overrides::describe(active[0]) ==
          "AddTwo at 0x08000100 arm -> test_add_two [test]: off (disabled in the config)");
    active = anygba::overrides::activate(game, Mode::All);
    CHECK(active[0].on);
    CHECK(g_on[0] == 1);
    CHECK(anygba::overrides::describe(active[0]) == "AddTwo at 0x08000100 arm -> test_add_two [test]: on");
    anygba::overrides::activate(game, Mode::Off);
    CHECK(g_on[0] == 0);

    const anygba_override_entry unknown{kOverridden, 0, "no_such_impl", "AddTwo", "", 1, original};
    CHECK_THROWS_WITH_AS(anygba::overrides::activate(game_with(&unknown), Mode::Off),
                         doctest::Contains("override AddTwo at 0x08000100: no implementation 'no_such_impl'"),
                         anygba::Error);

    CHECK(anygba::overrides::parse_mode("all") == Mode::All);
    CHECK_FALSE(anygba::overrides::parse_mode("yes"));
}

TEST_CASE("overrides: calls from recompiled code and from the interpreter reach the override; off runs the original") {
    const anygba_override_entry entry{kOverridden, 0, "test_add_two", "AddTwo", "", 1, original};
    const anygba_game game = game_with(&entry);
    anygba::overrides::activate(game, anygba::overrides::Mode::On);
    anygba::System sys(test_rom(), &game, quiet());
    anygba_cpu& c = sys.cpu();

    g_calls = 0;
    c.r[0] = 40;
    call_from_recompiled_code(c);
    CHECK(g_calls == 1);
    CHECK(g_from_recompiled);
    CHECK(c.r[0] == 42);  // two guest calls, interpreted
    CHECK(c.r[15] == kCallSite);
    CHECK(sys.memory().ewram[0] == 0xEF);
    CHECK(sys.memory().ewram[1] == 0xBE);

    // bl from interpreted code: the interpreter stops at the compiled entry, the dispatcher runs it.
    c.r[15] = 0x08000000;
    c.t = 0;
    c.r[0] = 1;
    sys.interpret();
    CHECK(c.r[15] == kOverridden);
    sys.run_once();
    CHECK(g_calls == 2);
    CHECK_FALSE(g_from_recompiled);
    CHECK(c.r[0] == 3);
    CHECK(c.r[15] == 0x08000004);

    anygba::overrides::activate(game, anygba::overrides::Mode::Off);
    call_from_recompiled_code(c);
    CHECK(g_calls == 2);
    CHECK(c.r[0] == 99);
}

TEST_CASE("overrides: run_original() wraps the translation; finishing without a return is an error") {
    const anygba_override_entry wraps{kOverridden, 0, "test_wraps_original", "AddTwo", "", 1, original};
    const anygba_game wrapping = game_with(&wraps);
    anygba::overrides::activate(wrapping, anygba::overrides::Mode::On);
    anygba::System sys(test_rom(), &wrapping, quiet());
    g_calls = 0;
    call_from_recompiled_code(sys.cpu());
    CHECK(g_calls == 1);
    CHECK(sys.cpu().r[0] == 99);
    CHECK(sys.cpu().r[15] == kCallSite);

    const anygba_override_entry broken{kOverridden, 0, "test_forgets_to_return", "AddTwo", "", 1, original};
    const anygba_game game = game_with(&broken);
    anygba::overrides::activate(game, anygba::overrides::Mode::On);
    // Inside run(), as in a game: the failure stops the run loop (anygba_override_run is extern "C",
    // so it must not throw through generated code).
    sys.cpu().r[15] = kOverridden;
    sys.cpu().t = 0;
    CHECK_THROWS_WITH_AS(sys.run(), doctest::Contains("finished without ret(), ret_to() or run_original()"),
                         anygba::Error);
    anygba::overrides::activate(game, anygba::overrides::Mode::Off);
}
