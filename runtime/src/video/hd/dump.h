#pragma once

#include "video/hd/renderer.h"

#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

// Dump mode (docs/user/HD-PACKS.md): every distinct tile, sprite and bitmap frame the game shows
// is written once as a 1x PNG named by its key, with index.json listing where and how often it
// was seen. A dump folder is also a valid (identity) HD pack.
namespace anygba::video::hd {

struct DumpFilter {
    bool tiles = true;    // 8x8 BG tiles (text and affine)
    bool objs = true;     // whole sprites
    bool bitmaps = true;  // mode 3/4/5 frames
    std::array<bool, 4> bg{true, true, true, true};
    bool blank = false;   // also dump fully transparent art
    std::size_t max_entries = 1'000'000;
};

// "tiles,objs,bitmaps,bg0,bg1,bg2,bg3,blank" (any subset; "all" = everything but blank).
// Returns an error message.
std::optional<std::string> parse_dump_filter(std::string_view spec, DumpFilter& filter);

class Dumper final : public ArtSink {
public:
    // Continues a dump already in `dir` (its index.json). Throws std::runtime_error when the
    // folder cannot be created or the index is damaged.
    Dumper(std::filesystem::path dir, std::string game_code, std::string title, DumpFilter filter = {});
    ~Dumper() override;  // writes the index and waits for pending images
    Dumper(const Dumper&) = delete;
    Dumper& operator=(const Dumper&) = delete;

    bool add(KeyType type, std::uint64_t key, std::uint64_t index_key, const Art& art, const char* layer,
             std::uint64_t frame) override;
    void context(KeyType type, std::uint64_t key, const Art& context, int x, int y) override;

    // Writes index.json now (the owner calls this every few seconds). Returns an error message.
    std::optional<std::string> write_index();
    // Blocks until every queued image is written. Returns the first write error, if any.
    std::optional<std::string> wait();
    std::size_t entries() const;
    const std::filesystem::path& dir() const { return dir_; }

    static constexpr const char* kIndexFile = "index.json";
    // Subfolder of each kind of art inside a dump.
    static std::string folder(KeyType type);

private:
    struct Entry {
        std::uint64_t index_key = 0;
        int width = 0, height = 0;
        std::string layer;
        std::uint64_t first_frame = 0, last_frame = 0, frames = 0;
    };
    struct Job {
        std::filesystem::path path;
        int width, height;
        std::vector<std::uint32_t> pixels;
    };
    void load_index();
    void enqueue(std::filesystem::path path, const Art& art);
    void writer();

    std::filesystem::path dir_;
    std::string game_code_, title_;
    DumpFilter filter_;

    mutable std::mutex mutex_;
    std::map<std::pair<int, std::uint64_t>, Entry> entries_;
    bool dirty_ = false;

    std::mutex jobs_mutex_;
    std::condition_variable jobs_cv_, idle_cv_;
    std::deque<Job> jobs_;
    bool busy_ = false;
    bool stop_ = false;
    std::optional<std::string> error_;
    std::thread thread_;
};

}  // namespace anygba::video::hd
