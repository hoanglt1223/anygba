#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace anygba::test {

std::filesystem::path corpus_path(const std::filesystem::path& relative);

// Contents of a corpus file, or nothing if the corpus has not been fetched.
std::optional<std::vector<std::uint8_t>> read_corpus_file(const std::filesystem::path& relative);

std::vector<std::uint8_t> read_file(const std::filesystem::path& path);

}  // namespace anygba::test
