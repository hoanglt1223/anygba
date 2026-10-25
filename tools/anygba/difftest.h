#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// `anygba difftest`: runs a generated game's differential test of one override
// (docs/dev/OVERRIDES.md) and, after a pass, can record it in the module's overrides.toml.
namespace anygba::driver {

std::string difftest_usage();
// Returns 0 when the difftest passed, 1 when it failed, 2 for bad arguments or errors.
int difftest(std::span<const std::string_view> args);

// Sets `verified = true` (with `comment`) in the [[override]] of `impl` in an overrides.toml,
// keeping the rest of the file. Throws std::runtime_error when no entry has that impl.
void mark_verified(const std::filesystem::path& toml, const std::string& impl, const std::string& comment);

}  // namespace anygba::driver
