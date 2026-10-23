#pragma once

#include <stdexcept>

namespace anyport::platform {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ContextOptions {
    // Use SDL's dummy video/audio drivers and the software renderer (CI, frame dumps).
    bool headless = false;
};

// Owns SDL initialisation for video, audio, and gamepads. Only one may exist at a time,
// and every other platform object must be destroyed before it.
class Context {
public:
    explicit Context(const ContextOptions& options = {});
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    bool headless() const noexcept { return headless_; }

private:
    bool headless_;
};

}  // namespace anyport::platform
