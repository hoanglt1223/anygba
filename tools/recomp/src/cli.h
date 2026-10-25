#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anygba::recomp {

struct Options {
    std::filesystem::path rom;
    std::optional<std::filesystem::path> config;
    std::filesystem::path output_dir = "recomp-out";
    std::vector<std::filesystem::path> miss_logs;
    std::vector<std::filesystem::path> override_modules;  // folders with overrides.toml and sources
    std::vector<std::filesystem::path> symbol_files;      // [functions] tables for override symbols
    std::size_t shards = 0;  // 0: one funcs_NNN.c per 500 functions
    bool list_only = false;  // print discovered functions and disassembly instead of generating C
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
