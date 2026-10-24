#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace anygba::video::hd {

// Runs a program with arguments (no shell, no console window) and waits for it. Returns an
// error message when it cannot start or exits with a non-zero status. While it runs, `tick` is
// called about five times a second, and once `cancel` is set the program is killed and the
// result is kCancelled.
std::optional<std::string> run_process(const std::vector<std::string>& args, const std::atomic<bool>* cancel = nullptr,
                                       const std::function<void()>& tick = {});

inline constexpr const char* kCancelled = "cancelled";

}  // namespace anygba::video::hd
