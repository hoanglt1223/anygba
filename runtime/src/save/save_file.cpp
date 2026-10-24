#include "save/save.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <thread>

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

namespace {

// Sharing and lock violations, and the access denied Windows reports while another program
// holds a file open without delete sharing.
bool transient(const std::error_code& ec) {
#if defined(_WIN32)
    if (ec.category() == std::system_category()) {
        return ec.value() == 32 || ec.value() == 33 || ec.value() == 5;
    }
#endif
    return ec == std::errc::device_or_resource_busy || ec == std::errc::permission_denied;
}

// Runs `step` until it succeeds, fails for another reason, or about 0.6 s has passed.
template <typename Step>
void with_retry(Step step, std::error_code& ec) {
    int delay_ms = 20;
    for (int attempt = 0;; ++attempt) {
        ec.clear();
        step(ec);
        if (!ec || !transient(ec) || attempt == 5) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        delay_ms *= 2;
    }
}

}  // namespace

std::optional<std::string> replace_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                        bool keep_backup) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    with_retry(
        [&](std::error_code& e) {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            out.close();
            // ofstream gives no error code; treat a failed open or write like a lock.
            if (!out) e = std::make_error_code(std::errc::device_or_resource_busy);
        },
        ec);
    if (ec) {
        fs::remove(tmp, ec);
        return "cannot write " + tmp.string();
    }
    if (keep_backup && fs::exists(path, ec)) {
        auto bak = path;
        bak += ".bak";
        with_retry([&](std::error_code& e) { fs::copy_file(path, bak, fs::copy_options::overwrite_existing, e); }, ec);
        if (ec) return "cannot back up " + path.string() + ": " + ec.message();
    }
    with_retry([&](std::error_code& e) { fs::rename(tmp, path, e); }, ec);
    if (ec) return "cannot replace " + path.string() + ": " + ec.message();
    return std::nullopt;
}

std::optional<std::string> SaveFile::write() {
    const std::uint64_t gen = backup_.generation();
    // Keep the previous save: a wrong save type or size guess must never destroy it.
    if (auto error = replace_file(path_, backup_.data(), true)) return error;
    written_generation_ = gen;
    return std::nullopt;
}

}  // namespace anygba::save
