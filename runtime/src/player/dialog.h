#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

// The system's file and folder pickers (SDL3 dialogs), polled from the player's frame loop.
// SDL may answer on another thread, so the answer is handed over through a shared slot.
namespace anygba::player {

class FileDialog {
public:
    enum class Kind : std::uint8_t { Folder, Open, Save };

    // Shows a picker. `location` is the starting folder or file (UTF-8, may be empty);
    // `pattern` is a file pattern such as "sav" for Open and Save. Returns false if one is
    // already open.
    bool show(Kind kind, const std::string& location, const char* filter_name = nullptr, const char* pattern = nullptr);
    bool busy() const;
    // The answer once the picker has closed: the chosen path (UTF-8), or "" if cancelled
    // or failed. nullopt while it is still open or none was shown.
    std::optional<std::string> take();

private:
    struct Slot {
        std::mutex mutex;
        bool open = false;
        std::optional<std::string> answer;
    };
    std::shared_ptr<Slot> slot_ = std::make_shared<Slot>();
};

}  // namespace anygba::player
