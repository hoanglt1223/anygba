#include "support/corpus.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace anygba::test {

std::filesystem::path corpus_path(const std::filesystem::path& relative) {
    return std::filesystem::path(ANYGBA_CORPUS_DIR) / relative;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::optional<std::vector<std::uint8_t>> read_corpus_file(const std::filesystem::path& relative) {
    const auto path = corpus_path(relative);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    return read_file(path);
}

}  // namespace anygba::test
