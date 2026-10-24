#pragma once

#include "video/hd/renderer.h"

#include <array>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

// An HD pack folder (docs/user/HD-PACKS.md): PNG files named by key ("t-0123456789abcdef.png"),
// in any subfolders, at any whole-number scale from 1x to 16x of the art they replace, and an
// optional pack.toml. Images are read on first use and resampled to the internal scale.
namespace anygba::video::hd {

class Pack final : public TextureSource {
public:
    // Indexes `dir`. Throws std::runtime_error when it is not a folder or pack.toml is invalid.
    explicit Pack(std::filesystem::path dir);
    ~Pack() override;
    Pack(const Pack&) = delete;
    Pack& operator=(const Pack&) = delete;

    bool has(KeyType type) const override;
    const Texture* find(KeyType type, std::uint64_t hash, int width, int height, int scale) override;

    // Hot reload: a background thread rescans the folder every `interval_ms`; apply_changes()
    // (between frames) swaps in a changed index and drops decoded art. Returns true if it did.
    void watch(int interval_ms = 1000);
    bool apply_changes();
    // Rescans now and applies any change. Returns true if the pack changed.
    bool reload();

    const std::filesystem::path& dir() const { return dir_; }
    const std::string& title() const { return title_; }
    std::size_t size() const;
    // Problems met so far (bad images, wrong sizes), each reported once.
    std::vector<std::string> take_warnings();

private:
    struct File {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
        std::uintmax_t size = 0;
        bool operator==(const File&) const = default;
    };
    using Index = std::array<std::unordered_map<std::uint64_t, File>, kKeyTypes>;
    using CacheKey = std::tuple<int, std::uint64_t, int>;  // type, hash, scale (0: as stored)

    Index scan() const;
    void install(Index index);
    const Texture* original(KeyType type, std::uint64_t hash, const File& file);
    void warn(std::string message);

    std::filesystem::path dir_;
    std::string title_;
    bool linear_upscale_ = false;

    mutable std::mutex mutex_;
    Index index_;
    std::map<CacheKey, std::shared_ptr<const Texture>> cache_;  // null: unusable
    std::vector<std::string> warnings_;

    std::thread watcher_;
    std::mutex watch_mutex_;
    std::condition_variable watch_cv_;
    bool stop_ = false;
    std::unique_ptr<Index> pending_;
};

// Resamples RGBA art to width x height: box-filtered down by whole factors, bilinear for other
// reductions, nearest (or bilinear with linear_up) up. Colours are weighted by alpha.
Texture resample(const Texture& src, int width, int height, bool linear_up);

}  // namespace anygba::video::hd
