#include "player/lifecycle.h"

#include <SDL3/SDL_events.h>

#include <cstdio>
#include <exception>

namespace anygba::player {

BackgroundWatch::BackgroundWatch(std::function<void()> on_background) : on_background_(std::move(on_background)) {
    if (!SDL_AddEventWatch(&BackgroundWatch::on_event, this)) {
        std::fprintf(stderr, "warning: saves are not written when the app is backgrounded: %s\n", SDL_GetError());
    }
}

BackgroundWatch::~BackgroundWatch() { SDL_RemoveEventWatch(&BackgroundWatch::on_event, this); }

bool BackgroundWatch::on_event(void* self, SDL_Event* event) {
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_TERMINATING) {
        try {
            static_cast<BackgroundWatch*>(self)->on_background_();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
        }
    }
    return true;
}

}  // namespace anygba::player
