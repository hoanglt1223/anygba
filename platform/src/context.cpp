#include <anyport/platform/context.h>

#include "sdl_error.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>

#include <atomic>

namespace anyport::platform {

namespace {

std::atomic<bool> g_context_alive{false};

void apply_driver_hints(bool headless) {
    if (headless) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    } else {
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
        SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
        SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
    }
}

}  // namespace

Context::Context(const ContextOptions& options) : headless_(options.headless) {
    if (g_context_alive.exchange(true)) {
        throw Error("a platform Context already exists");
    }
    apply_driver_hints(headless_);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        g_context_alive = false;
        detail::throw_sdl_error("SDL_Init");
    }
}

Context::~Context() {
    SDL_Quit();
    g_context_alive = false;
}

}  // namespace anyport::platform
