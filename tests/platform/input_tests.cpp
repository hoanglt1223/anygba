#include <anyport/platform/context.h>
#include <anyport/platform/input.h>

#include <SDL3/SDL_events.h>

#include <doctest.h>

namespace pf = anyport::platform;

namespace {

SDL_Event key_event(SDL_Scancode scancode, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode;
    event.key.down = down;
    return event;
}

}  // namespace

TEST_CASE("default keyboard bindings drive the virtual pad") {
    pf::Context context({.headless = true});
    pf::Input input;
    CHECK(input.state().buttons == 0);

    input.handle_event(key_event(SDL_SCANCODE_X, true));
    input.handle_event(key_event(SDL_SCANCODE_LEFT, true));
    auto state = input.state();
    CHECK(state.held(pf::Button::South));
    CHECK(state.held(pf::Button::DpadLeft));
    CHECK_FALSE(state.held(pf::Button::East));
    CHECK(input.key_held("X"));

    input.handle_event(key_event(SDL_SCANCODE_X, false));
    state = input.state();
    CHECK_FALSE(state.held(pf::Button::South));
    CHECK(state.held(pf::Button::DpadLeft));
}

TEST_CASE("losing focus releases every key") {
    pf::Context context({.headless = true});
    pf::Input input;
    input.handle_event(key_event(SDL_SCANCODE_RETURN, true));
    REQUIRE(input.state().held(pf::Button::Start));

    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    input.handle_event(focus);
    CHECK(input.state().buttons == 0);
}

TEST_CASE("custom key bindings") {
    pf::Context context({.headless = true});
    pf::Input input;

    SUBCASE("unknown names are rejected") {
        CHECK_FALSE(input.bind_key("NoSuchKey", pf::Button::South));
        CHECK_FALSE(input.bind_key("", pf::Button::South));
        CHECK_FALSE(input.bind_key("X", pf::Button::Count));
        CHECK_FALSE(input.key_held("NoSuchKey"));
    }

    SUBCASE("clearing then rebinding replaces the defaults") {
        input.clear_key_bindings();
        REQUIRE(input.bind_key("Left Shift", pf::Button::Back));
        input.handle_event(key_event(SDL_SCANCODE_X, true));
        input.handle_event(key_event(SDL_SCANCODE_LSHIFT, true));
        const auto state = input.state();
        CHECK_FALSE(state.held(pf::Button::South));
        CHECK(state.held(pf::Button::Back));
    }

    SUBCASE("one key may drive several buttons") {
        REQUIRE(input.bind_key("X", pf::Button::RightShoulder));
        input.handle_event(key_event(SDL_SCANCODE_X, true));
        const auto state = input.state();
        CHECK(state.held(pf::Button::South));
        CHECK(state.held(pf::Button::RightShoulder));
    }
}

TEST_CASE("pump_events routes queued events and reports quit") {
    pf::Context context({.headless = true});
    pf::Input input;

    SDL_Event down = key_event(SDL_SCANCODE_Z, true);
    REQUIRE(SDL_PushEvent(&down));
    CHECK(pf::pump_events(input));
    CHECK(input.state().held(pf::Button::East));

    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    REQUIRE(SDL_PushEvent(&quit));
    CHECK_FALSE(pf::pump_events(input));
}

TEST_CASE("only one context may exist") {
    pf::Context context({.headless = true});
    CHECK_THROWS_AS((pf::Context{{.headless = true}}), pf::Error);
}
