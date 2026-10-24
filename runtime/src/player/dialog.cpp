#include "player/dialog.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_keyboard.h>

namespace anygba::player {

namespace {

// Owned by the pending dialog; SDL needs the filter alive until the callback runs.
struct Request {
    std::shared_ptr<void> keep;
    std::mutex* mutex = nullptr;
    bool* open = nullptr;
    std::optional<std::string>* answer = nullptr;
    SDL_DialogFileFilter filter{};
};

void SDLCALL on_answer(void* userdata, const char* const* files, int /*filter*/) {
    auto* request = static_cast<Request*>(userdata);
    {
        std::lock_guard lock(*request->mutex);
        *request->answer = files && files[0] ? std::string(files[0]) : std::string();
        *request->open = false;
    }
    delete request;
}

}  // namespace

bool FileDialog::show(Kind kind, const std::string& location, const char* filter_name, const char* pattern) {
    {
        std::lock_guard lock(slot_->mutex);
        if (slot_->open) return false;
        slot_->open = true;
        slot_->answer.reset();
    }
    auto* request = new Request;
    request->keep = slot_;
    request->mutex = &slot_->mutex;
    request->open = &slot_->open;
    request->answer = &slot_->answer;
    const bool filtered = filter_name && pattern;
    if (filtered) request->filter = SDL_DialogFileFilter{filter_name, pattern};
    const char* start = location.empty() ? nullptr : location.c_str();  // SDL copies it
    SDL_Window* parent = SDL_GetKeyboardFocus();
    switch (kind) {
    case Kind::Folder: SDL_ShowOpenFolderDialog(on_answer, request, parent, start, false); break;
    case Kind::Open:
        SDL_ShowOpenFileDialog(on_answer, request, parent, filtered ? &request->filter : nullptr, filtered ? 1 : 0, start,
                               false);
        break;
    case Kind::Save:
        SDL_ShowSaveFileDialog(on_answer, request, parent, filtered ? &request->filter : nullptr, filtered ? 1 : 0, start);
        break;
    }
    return true;
}

bool FileDialog::busy() const {
    std::lock_guard lock(slot_->mutex);
    return slot_->open;
}

std::optional<std::string> FileDialog::take() {
    std::lock_guard lock(slot_->mutex);
    auto answer = std::move(slot_->answer);
    slot_->answer.reset();
    return answer;
}

}  // namespace anygba::player
