#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace anygba::recomp {

struct Options {
    std::filesystem::path rom;
    std::optional<std::filesystem::path> config;
    std::filesystem::path output_dir = "recomp-out";
};

enum class Action { Run, Help, Version, Error };

struct ParseResult {
    Action action = Action::Error;
    Options options;
    std::string error;
};

// `args` excludes the program name.
ParseResult parse_args(std::span<const std::string_view> args);

std::string usage();
std::string version();

// Checks that the inputs named in `options` exist. Returns an error message or nothing.
std::optional<std::string> validate_inputs(const Options& options);

}  // namespace anygba::recomp
