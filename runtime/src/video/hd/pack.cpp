#include "video/hd/pack.h"

#include "video/hd/png_io.h"

#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <system_error>

namespace anygba::video::hd {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

bool is_png(const fs::path& p) {
    std::string ext = utf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".png";
}

struct Premul {
    double r = 0, g = 0, b = 0, a = 0;
    void add(std::uint32_t c, double w) {
        const double alpha = static_cast<double>(c >> 24) / 255.0 * w;
        r += static_cast<double>(c & 255u) * alpha;
        g += static_cast<double>((c >> 8) & 255u) * alpha;
        b += static_cast<double>((c >> 16) & 255u) * alpha;
        a += alpha;
    }
    std::uint32_t pixel(double total_weight) const {
        if (a <= 0) return 0;
        const auto ch = [&](double v) { return static_cast<std::uint32_t>(std::clamp(v / a + 0.5, 0.0, 255.0)); };
        const auto alpha = static_cast<std::uint32_t>(std::clamp(a / total_weight * 255.0 + 0.5, 0.0, 255.0));
        return ch(r) | (ch(g) << 8) | (ch(b) << 16) | (alpha << 24);
    }
};

std::uint32_t bilinear(const Texture& s, double x, double y) {
    x = std::clamp(x, 0.0, static_cast<double>(s.width - 1));
    y = std::clamp(y, 0.0, static_cast<double>(s.height - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, s.width - 1), y1 = std::min(y0 + 1, s.height - 1);
    const double fx = x - x0, fy = y - y0;
    Premul p;
    p.add(s.at(x0, y0), (1 - fx) * (1 - fy));
    p.add(s.at(x1, y0), fx * (1 - fy));
    p.add(s.at(x0, y1), (1 - fx) * fy);
    p.add(s.at(x1, y1), fx * fy);
    return p.pixel(1.0);
}

}  // namespace

Texture resample(const Texture& src, int width, int height, bool linear_up) {
    if (src.width == width && src.height == height) return src;
    Texture out;
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const bool down = width < src.width;
    const bool whole = down && src.width % width == 0 && src.height % height == 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint32_t c;
            if (whole) {
                const int fx = src.width / width, fy = src.height / height;
                Premul p;
                for (int sy = 0; sy < fy; ++sy) {
                    for (int sx = 0; sx < fx; ++sx) p.add(src.at(x * fx + sx, y * fy + sy), 1.0);
                }
                c = p.pixel(static_cast<double>(fx * fy));
            } else if (down || linear_up) {
                const double sx = (x + 0.5) * src.width / width - 0.5;
                const double sy = (y + 0.5) * src.height / height - 0.5;
                c = bilinear(src, sx, sy);
            } else {
                c = src.at(static_cast<int>(static_cast<long long>(x) * src.width / width),
                           static_cast<int>(static_cast<long long>(y) * src.height / height));
            }
            out.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = c;
        }
    }
    return out;
}

Pack::Pack(fs::path dir) : dir_(std::move(dir)) {
    std::error_code ec;
    if (!fs::is_directory(dir_, ec)) throw std::runtime_error("HD pack folder not found: " + utf8(dir_));
    const fs::path settings = dir_ / "pack.toml";
    if (fs::exists(settings, ec)) {
        toml::table t;
        try {
            t = toml::parse_file(settings.u8string());
        } catch (const toml::parse_error& e) {
            throw std::runtime_error(utf8(settings) + ":" + std::to_string(e.source().begin.line) + ": " +
                                     std::string(e.description()));
        }
        title_ = t["title"].value_or(std::string{});
        const std::string up = t["upscale"].value_or(std::string("nearest"));
        if (up != "nearest" && up != "linear") {
            throw std::runtime_error(utf8(settings) + ": upscale must be \"nearest\" or \"linear\"");
        }
        linear_upscale_ = up == "linear";
    }
    install(scan());
}

Pack::~Pack() {
    {
        std::lock_guard lock(watch_mutex_);
        stop_ = true;
    }
    watch_cv_.notify_all();
    if (watcher_.joinable()) watcher_.join();
}

Pack::Index Pack::scan() const {
    Index index;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir_, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::directory_entry& e = *it;
        std::error_code fe;
        if (!e.is_regular_file(fe) || !is_png(e.path())) continue;
        const auto key = parse_key(utf8(e.path().stem()));
        if (!key) continue;  // other PNGs (previews, context pictures) are not pack art
        File f{e.path(), e.last_write_time(fe), e.file_size(fe)};
        auto& slot = index[static_cast<std::size_t>(key->type)];
        const auto [pos, inserted] = slot.emplace(key->hash, f);
        if (!inserted && f.path < pos->second.path) pos->second = f;  // the same key twice: first path wins
    }
    return index;
}

void Pack::install(Index index) {
    std::lock_guard lock(mutex_);
    index_ = std::move(index);
    cache_.clear();
}

bool Pack::has(KeyType type) const {
    std::lock_guard lock(mutex_);
    return !index_[static_cast<std::size_t>(type)].empty();
}

std::size_t Pack::size() const {
    std::lock_guard lock(mutex_);
    std::size_t n = 0;
    for (const auto& m : index_) n += m.size();
    return n;
}

void Pack::warn(std::string message) {
    if (std::find(warnings_.begin(), warnings_.end(), message) == warnings_.end()) warnings_.push_back(std::move(message));
}

std::vector<std::string> Pack::take_warnings() {
    std::lock_guard lock(mutex_);
    return std::exchange(warnings_, {});
}

const Texture* Pack::find(KeyType type, std::uint64_t hash, int width, int height, int scale) {
    std::lock_guard lock(mutex_);
    const auto& slot = index_[static_cast<std::size_t>(type)];
    const auto file = slot.find(hash);
    if (file == slot.end()) return nullptr;
    const CacheKey key{static_cast<int>(type), hash, scale};
    if (const auto it = cache_.find(key); it != cache_.end()) return it->second.get();

    std::shared_ptr<const Texture> original;
    const CacheKey stored{static_cast<int>(type), hash, 0};
    if (const auto it = cache_.find(stored); it != cache_.end()) {
        original = it->second;
    } else {
        std::string error;
        if (auto t = load_png(file->second.path, &error)) {
            const int s = t->width / width;
            if (t->width % width != 0 || t->height % height != 0 || t->height / height != s || s < 1 || s > 16) {
                warn(utf8(file->second.path) + ": " + std::to_string(t->width) + "x" + std::to_string(t->height) +
                     " is not a 1x-16x multiple of the " + std::to_string(width) + "x" + std::to_string(height) +
                     " original; ignored");
            } else {
                original = std::make_shared<const Texture>(std::move(*t));
            }
        } else {
            warn(error);
        }
        cache_[stored] = original;
    }
    std::shared_ptr<const Texture> result;
    if (original) {
        if (original->width == width * scale && original->height == height * scale) {
            result = original;
        } else {
            result = std::make_shared<const Texture>(resample(*original, width * scale, height * scale, linear_upscale_));
        }
    }
    cache_[key] = result;
    return result.get();
}

void Pack::watch(int interval_ms) {
    if (watcher_.joinable()) return;
    watcher_ = std::thread([this, interval_ms] {
        std::unique_lock lock(watch_mutex_);
        while (!stop_) {
            if (watch_cv_.wait_for(lock, std::chrono::milliseconds(interval_ms), [this] { return stop_; })) break;
            lock.unlock();
            Index fresh = scan();
            bool changed;
            {
                std::lock_guard index_lock(mutex_);
                changed = fresh != index_;
            }
            lock.lock();
            if (changed) pending_ = std::make_unique<Index>(std::move(fresh));
        }
    });
}

bool Pack::apply_changes() {
    std::unique_ptr<Index> fresh;
    {
        std::lock_guard lock(watch_mutex_);
        fresh = std::move(pending_);
    }
    if (!fresh) return false;
    install(std::move(*fresh));
    return true;
}

bool Pack::reload() {
    Index fresh = scan();
    {
        std::lock_guard lock(mutex_);
        if (fresh == index_) return false;
    }
    install(std::move(fresh));
    return true;
}

}  // namespace anygba::video::hd
