#include "video/hd/dump.h"

#include "video/hd/json.h"
#include "video/hd/png_io.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace anygba::video::hd {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

const char* kind_name(KeyType t) {
    switch (t) {
    case KeyType::Tile: return "tile";
    case KeyType::Obj: return "obj";
    case KeyType::Bitmap: return "bitmap";
    default: return "?";
    }
}

std::optional<KeyType> kind_from(const std::string& s) {
    if (s == "tile") return KeyType::Tile;
    if (s == "obj") return KeyType::Obj;
    if (s == "bitmap") return KeyType::Bitmap;
    return std::nullopt;
}

KeyType index_type(KeyType t) { return t == KeyType::Obj ? KeyType::ObjIndices : KeyType::TileIndices; }

bool blank(const Art& art) {
    for (const std::uint16_t c : art.colors) {
        if (!(c & kClear)) return false;
    }
    return true;
}

}  // namespace

std::optional<std::string> parse_dump_filter(std::string_view spec, DumpFilter& f) {
    DumpFilter out;
    out.tiles = out.objs = out.bitmaps = false;
    out.bg = {false, false, false, false};
    bool any_bg = false;
    while (!spec.empty()) {
        const auto comma = spec.find(',');
        const std::string_view part = spec.substr(0, comma);
        spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
        if (part == "all") {
            out.tiles = out.objs = out.bitmaps = true;
        } else if (part == "tiles") {
            out.tiles = true;
        } else if (part == "objs") {
            out.objs = true;
        } else if (part == "bitmaps") {
            out.bitmaps = true;
        } else if (part == "blank") {
            out.blank = true;
        } else if (part.size() == 3 && part.substr(0, 2) == "bg" && part[2] >= '0' && part[2] <= '3') {
            out.bg[static_cast<std::size_t>(part[2] - '0')] = true;
            any_bg = true;
        } else {
            return "unknown dump filter '" + std::string(part) + "' (all, tiles, objs, bitmaps, bg0-bg3, blank)";
        }
    }
    if (!any_bg) out.bg = {true, true, true, true};
    if (!out.tiles && !out.objs && !out.bitmaps) out.tiles = out.objs = out.bitmaps = true;
    out.max_entries = f.max_entries;
    f = out;
    return std::nullopt;
}

std::string Dumper::folder(KeyType type) {
    switch (type) {
    case KeyType::Obj:
    case KeyType::ObjIndices: return "objs";
    case KeyType::Bitmap: return "bitmaps";
    default: return "tiles";
    }
}

Dumper::Dumper(fs::path dir, std::string game_code, std::string title, DumpFilter filter)
    : dir_(std::move(dir)), game_code_(std::move(game_code)), title_(std::move(title)), filter_(filter) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (!fs::is_directory(dir_, ec)) throw std::runtime_error("cannot create the dump folder " + utf8(dir_));
    load_index();
    thread_ = std::thread([this] { writer(); });
}

Dumper::~Dumper() {
    if (const auto error = write_index()) std::fprintf(stderr, "warning: %s\n", error->c_str());
    {
        std::lock_guard lock(jobs_mutex_);
        stop_ = true;
    }
    jobs_cv_.notify_all();
    thread_.join();
}

void Dumper::load_index() {
    const fs::path path = dir_ / kIndexFile;
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    std::ifstream in(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    json::Value doc;
    try {
        doc = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(utf8(path) + ": " + e.what());
    }
    const json::Value* list = doc.get("entries");
    if (!list || list->type != json::Value::Type::Array) throw std::runtime_error(utf8(path) + ": no \"entries\" list");
    for (const json::Value& v : list->array) {
        const auto key = parse_key(v.str("key"));
        const auto kind = kind_from(v.str("kind"));
        if (!key || !kind || key->type != *kind) throw std::runtime_error(utf8(path) + ": bad entry " + v.str("key"));
        Entry e;
        if (const auto ik = parse_key(v.str("indices"))) e.index_key = ik->hash;
        e.width = static_cast<int>(v.num("width"));
        e.height = static_cast<int>(v.num("height"));
        e.layer = v.str("layer");
        e.first_frame = static_cast<std::uint64_t>(v.num("first_frame"));
        e.last_frame = static_cast<std::uint64_t>(v.num("last_frame"));
        e.frames = static_cast<std::uint64_t>(v.num("frames"));
        entries_[{static_cast<int>(key->type), key->hash}] = e;
    }
}

bool Dumper::add(KeyType type, std::uint64_t key, std::uint64_t index_key, const Art& art, const char* layer,
                 std::uint64_t frame) {
    {
        std::lock_guard lock(mutex_);
        const auto id = std::make_pair(static_cast<int>(type), key);
        if (const auto it = entries_.find(id); it != entries_.end()) {
            if (it->second.last_frame != frame) {
                it->second.last_frame = frame;
                ++it->second.frames;
                dirty_ = true;
            }
            return false;
        }
        const bool wanted = type == KeyType::Tile     ? filter_.tiles
                            : type == KeyType::Obj    ? filter_.objs
                            : type == KeyType::Bitmap ? filter_.bitmaps
                                                      : false;
        if (!wanted || entries_.size() >= filter_.max_entries || (!filter_.blank && blank(art))) return false;
        if (layer[0] == 'b' && layer[1] == 'g' && !filter_.bg[static_cast<std::size_t>(layer[2] - '0') & 3]) return false;
        Entry e;
        e.index_key = index_key;
        e.width = art.width;
        e.height = art.height;
        e.layer = layer;
        e.first_frame = e.last_frame = frame;
        e.frames = 1;
        entries_.emplace(id, std::move(e));
        dirty_ = true;
    }
    enqueue(dir_ / folder(type) / (key_name(type, key) + ".png"), art);
    return true;
}

void Dumper::context(KeyType type, std::uint64_t key, const Art& context, int, int) {
    enqueue(dir_ / folder(type) / (key_name(type, key) + ".context.png"), context);
}

void Dumper::enqueue(fs::path path, const Art& art) {
    Job job{std::move(path), art.width, art.height, {}};
    job.pixels.reserve(art.colors.size());
    for (const std::uint16_t c : art.colors) job.pixels.push_back(expand(c));
    {
        std::lock_guard lock(jobs_mutex_);
        jobs_.push_back(std::move(job));
    }
    jobs_cv_.notify_one();
}

void Dumper::writer() {
    std::unique_lock lock(jobs_mutex_);
    while (true) {
        jobs_cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (jobs_.empty()) {
            if (stop_) break;
            continue;
        }
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        busy_ = true;
        lock.unlock();
        const auto error = save_png(job.path, job.pixels.data(), job.width, job.height);
        lock.lock();
        busy_ = false;
        if (error && !error_) error_ = error;
        if (jobs_.empty()) idle_cv_.notify_all();
    }
    idle_cv_.notify_all();
}

std::optional<std::string> Dumper::wait() {
    std::unique_lock lock(jobs_mutex_);
    idle_cv_.wait(lock, [this] { return jobs_.empty() && !busy_; });
    return error_;
}

std::size_t Dumper::entries() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

std::optional<std::string> Dumper::write_index() {
    std::string text;
    {
        std::lock_guard lock(mutex_);
        if (!dirty_ && fs::exists(dir_ / kIndexFile)) return std::nullopt;
        text = "{\n  \"format\": 1,\n  \"game\": " + json::quote(game_code_) + ",\n  \"title\": " + json::quote(title_) +
               ",\n  \"entries\": [";
        bool first = true;
        for (const auto& [id, e] : entries_) {
            const auto type = static_cast<KeyType>(id.first);
            text += first ? "\n" : ",\n";
            first = false;
            text += "    {\"key\": " + json::quote(key_name(type, id.second)) + ", \"kind\": " + json::quote(kind_name(type)) +
                    ", \"file\": " + json::quote(folder(type) + "/" + key_name(type, id.second) + ".png");
            if (e.index_key) text += ", \"indices\": " + json::quote(key_name(index_type(type), e.index_key));
            text += ", \"width\": " + std::to_string(e.width) + ", \"height\": " + std::to_string(e.height) +
                    ", \"layer\": " + json::quote(e.layer) + ", \"first_frame\": " + std::to_string(e.first_frame) +
                    ", \"last_frame\": " + std::to_string(e.last_frame) + ", \"frames\": " + std::to_string(e.frames) + "}";
        }
        text += "\n  ]\n}\n";
        dirty_ = false;
    }
    const fs::path path = dir_ / kIndexFile;
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out) return "cannot write " + utf8(tmp);
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) return "cannot replace " + utf8(path) + ": " + ec.message();
    return std::nullopt;
}

}  // namespace anygba::video::hd
