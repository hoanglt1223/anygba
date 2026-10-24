#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anygba::driver {

struct BuildOptions {
    std::filesystem::path rom;
    std::optional<std::filesystem::path> config;
    std::vector<std::filesystem::path> miss_logs;
    std::filesystem::path output_dir = ".";
    std::optional<std::filesystem::path> work_dir;  // default: <output>/<rom stem>-build
    std::optional<std::string> name;                // default: the game title
    std::optional<std::filesystem::path> prefix;    // anygba install prefix; default: next to this program
    std::string cmake = "cmake";
    std::optional<std::string> generator;
    std::string build_type = "Release";
    bool embed_rom = true;
};

// Parses the arguments after `build`. Returns an error message on bad input.
std::optional<std::string> parse_build_args(std::span<const std::string_view> args, BuildOptions& out);
std::string build_usage();

// Turns a title into a file name: letters, digits, '-', '_' and spaces kept, the rest
// dropped, trimmed; "game" if nothing is left.
std::string program_name(std::string_view title);

// Recompiles, configures and builds against the installed runtime, and writes the program
// to the output folder: <Name>.exe on Windows, <Name> on Linux, <Name>.app on macOS.
// Returns the path written. Throws std::runtime_error with a user-facing message.
std::filesystem::path build(const BuildOptions& options);

// Runs a program with arguments (no shell) and returns its exit code. Throws if it cannot start.
int run_process(const std::vector<std::string>& argv);

}  // namespace anygba::driver
