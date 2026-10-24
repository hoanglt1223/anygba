#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Starting a game for anygba-mcp and checking whether a game process is still alive.
namespace anygba::mcp {

// Starts `argv` detached from our stdio: stdin from the null device, stdout and stderr appended
// to `log`, no console window. Returns the process ID; throws std::runtime_error.
int spawn(const std::vector<std::string>& argv, const std::filesystem::path& log);
bool process_alive(int pid);

}  // namespace anygba::mcp
