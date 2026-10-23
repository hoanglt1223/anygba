#pragma once

#include <anyport/platform/context.h>

#include <SDL3/SDL_error.h>

#include <string>

namespace anyport::platform::detail {

[[noreturn]] inline void throw_sdl_error(const char* operation) {
    throw Error(std::string(operation) + " failed: " + SDL_GetError());
}

}  // namespace anyport::platform::detail
