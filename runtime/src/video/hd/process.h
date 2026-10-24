#pragma once

#include <optional>
#include <string>
#include <vector>

namespace anygba::video::hd {

// Runs a program with arguments (no shell, no console window) and waits for it. Returns an
// error message when it cannot start or exits with a non-zero status.
std::optional<std::string> run_process(const std::vector<std::string>& args);

}  // namespace anygba::video::hd
