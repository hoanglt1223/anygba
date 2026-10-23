#include <anyport/platform/log.h>

#include <SDL3/SDL_log.h>

#include <cstdarg>

namespace anyport::platform {

namespace {

SDL_LogPriority to_sdl(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return SDL_LOG_PRIORITY_DEBUG;
    case LogLevel::Info:
        return SDL_LOG_PRIORITY_INFO;
    case LogLevel::Warn:
        return SDL_LOG_PRIORITY_WARN;
    case LogLevel::Error:
        return SDL_LOG_PRIORITY_ERROR;
    }
    return SDL_LOG_PRIORITY_ERROR;
}

}  // namespace

void set_log_level(LogLevel level) {
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, to_sdl(level));
}

void log(LogLevel level, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, to_sdl(level), format, args);
    va_end(args);
}

}  // namespace anyport::platform
