#pragma once

#include <functional>

union SDL_Event;

namespace anygba::player {

// Calls `on_background` when the app is about to be suspended or terminated (Android
// onPause, iOS backgrounding). The system may kill a backgrounded app without warning, so
// this is the last safe moment to write saves. SDL delivers these events from inside its
// event pump on the game thread, between frames. Does nothing on desktop systems.
class BackgroundWatch {
public:
    explicit BackgroundWatch(std::function<void()> on_background);
    ~BackgroundWatch();

    BackgroundWatch(const BackgroundWatch&) = delete;
    BackgroundWatch& operator=(const BackgroundWatch&) = delete;

private:
    static bool on_event(void* self, SDL_Event* event);
    std::function<void()> on_background_;
};

}  // namespace anygba::player
