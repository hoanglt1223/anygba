#include "save/save.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace anygba::save {

SaveFile::SaveFile(std::filesystem::path path, Backup& backup, std::uint32_t debounce_frames)
    : path_(std::move(path)), backup_(backup), debounce_(debounce_frames) {
    written_generation_ = seen_generation_ = backup_.generation();
}

void SaveFile::load() {
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        return;
    }
    std::ifstream file(path_, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot read save file " + path_.string());
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    try {
        backup_.load(std::move(bytes));
    } catch (const std::exception& e) {
        throw std::runtime_error(path_.string() + ": " + e.what());
    }
    written_generation_ = seen_generation_ = backup_.generation();
}

std::optional<std::string> SaveFile::on_frame() {
    const std::uint64_t gen = backup_.generation();
    if (gen != seen_generation_) {
        seen_generation_ = gen;
        stable_frames_ = 0;
        return std::nullopt;
    }
    if (gen == written_generation_ || ++stable_frames_ < debounce_) {
        return std::nullopt;
    }
    return write();
}

std::optional<std::string> SaveFile::flush() {
    if (backup_.generation() == written_generation_) {
        return std::nullopt;
    }
    return write();
}

std::optional<std::string> SaveFile::write() {
    const std::uint64_t gen = backup_.generation();
    const std::vector<std::uint8_t>& bytes = backup_.data();
    std::error_code ec;
    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path(), ec);
    }
    auto tmp = path_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(tmp, ec);
            return "cannot write save file " + tmp.string();
        }
    }
    // Keep the previous save: a wrong save type or size guess must never destroy it.
    if (std::filesystem::exists(path_, ec)) {
        auto bak = path_;
        bak += ".bak";
        std::filesystem::copy_file(path_, bak, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return "cannot back up " + path_.string() + ": " + ec.message();
        }
    }
    std::filesystem::rename(tmp, path_, ec);
    if (ec) {
        return "cannot replace " + path_.string() + ": " + ec.message();
    }
    written_generation_ = gen;
    return std::nullopt;
}

}  // namespace anygba::save
