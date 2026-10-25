#include "hdpack.h"

#include "video/hd/ai.h"
#include "video/hd/dump.h"
#include "video/hd/json.h"
#include "video/hd/keys.h"
#include "video/hd/pack.h"
#include "video/hd/png_io.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace anygba::hdpack {

namespace fs = std::filesystem;
namespace hd = anygba::video::hd;
using anygba::video::Image;

namespace {

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

fs::path path_arg(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

struct Entry {
    hd::KeyType type = hd::KeyType::Tile;
    std::uint64_t key = 0;
    std::string name;  // "t-0123..."
    fs::path file;     // absolute
    std::string indices;
    int width = 0, height = 0;
    std::string layer;
    std::uint64_t frames = 0;
};

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + utf8(p));
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The entries of a dump (index.json), or of any folder of key-named PNGs.
std::vector<Entry> entries_of(const fs::path& dir) {
    std::vector<Entry> out;
    std::error_code ec;
    if (fs::exists(dir / hd::Dumper::kIndexFile, ec)) {
        const auto doc = hd::json::parse(read_text(dir / hd::Dumper::kIndexFile));
        const auto* list = doc.get("entries");
        if (!list) throw std::runtime_error(utf8(dir / hd::Dumper::kIndexFile) + ": no entries");
        for (const auto& v : list->array) {
            const auto k = hd::parse_key(v.str("key"));
            if (!k) continue;
            Entry e;
            e.type = k->type;
            e.key = k->hash;
            e.name = v.str("key");
            e.file = dir / path_arg(v.str("file"));
            e.indices = v.str("indices");
            e.width = static_cast<int>(v.num("width"));
            e.height = static_cast<int>(v.num("height"));
            e.layer = v.str("layer");
            e.frames = static_cast<std::uint64_t>(v.num("frames"));
            out.push_back(std::move(e));
        }
        return out;
    }
    if (!fs::is_directory(dir, ec)) throw std::runtime_error("not a folder: " + utf8(dir));
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".png") continue;
        const auto k = hd::parse_key(utf8(it->path().stem()));
        if (!k) continue;
        Entry e;
        e.type = k->type;
        e.key = k->hash;
        e.name = utf8(it->path().stem());
        e.file = it->path();
        out.push_back(std::move(e));
    }
    return out;
}

hd::Texture load(const fs::path& p) {
    std::string error;
    auto t = hd::load_png(p, &error);
    if (!t) throw std::runtime_error(error);
    return std::move(*t);
}

Image to_image(const hd::Texture& t) {
    Image i;
    i.width = t.width;
    i.height = t.height;
    i.pixels = t.pixels;
    return i;
}

void save(const fs::path& p, const Image& i) {
    if (const auto e = hd::save_png(p, i)) throw std::runtime_error(*e);
}

std::optional<fs::path> data_dir() {
    char* p = SDL_GetPrefPath("anyport", "anygba");
    if (!p) return std::nullopt;
    fs::path r = path_arg(p);
    SDL_free(p);
    return r;
}

// Options of the form --name value; positional arguments kept in order.
struct Args {
    std::vector<std::string_view> positional;
    std::map<std::string, std::string> values;
    std::vector<std::string> flags;
    bool flag(const std::string& f) const { return std::find(flags.begin(), flags.end(), f) != flags.end(); }
    std::optional<std::string> value(const std::string& k) const {
        const auto it = values.find(k);
        return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
};

Args parse(std::span<const std::string_view> args, std::initializer_list<const char*> with_value) {
    Args a;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view s = args[i];
        if (s == "-o") {
            if (i + 1 >= args.size()) throw std::runtime_error("-o needs a value");
            a.values["o"] = std::string(args[++i]);
            continue;
        }
        if (!s.starts_with("--")) {
            a.positional.push_back(s);
            continue;
        }
        const std::string name(s.substr(2));
        if (std::find_if(with_value.begin(), with_value.end(), [&](const char* v) { return name == v; }) != with_value.end()) {
            if (i + 1 >= args.size()) throw std::runtime_error("--" + name + " needs a value");
            a.values[name] = std::string(args[++i]);
        } else {
            a.flags.push_back(name);
        }
    }
    return a;
}

int to_int(const std::string& s, const char* what, int lo, int hi) {
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (*end != '\0' || v < lo || v > hi) {
        throw std::runtime_error(std::string(what) + " must be a number from " + std::to_string(lo) + " to " + std::to_string(hi));
    }
    return static_cast<int>(v);
}

// ---- dedupe ----

int dedupe(const Args& a) {
    if (a.positional.size() != 1) throw std::runtime_error("usage: anygba hdpack dedupe DUMP [--apply]");
    const fs::path dir = path_arg(a.positional[0]);
    const auto entries = entries_of(dir);
    std::map<std::string, std::vector<const Entry*>> groups;  // by palette-independent key
    for (const Entry& e : entries) {
        if (!e.indices.empty()) groups[e.indices].push_back(&e);
    }
    std::size_t shared = 0, variants = 0;
    for (auto& [indices, list] : groups) {
        if (list.size() < 2) continue;
        ++shared;
        variants += list.size();
        std::sort(list.begin(), list.end(), [](const Entry* x, const Entry* y) { return x->frames > y->frames; });
        if (!a.flag("apply")) continue;
        // One image for every palette: the most used variant, named by its colour indices.
        const fs::path to = dir / "shared" / (indices + ".png");
        fs::create_directories(to.parent_path());
        fs::copy_file(list[0]->file, to, fs::copy_options::overwrite_existing);
        for (const Entry* e : list) {
            const fs::path moved = dir / "variants" / e->file.filename();
            fs::create_directories(moved.parent_path());
            std::error_code ec;
            fs::rename(e->file, moved, ec);
            if (ec) throw std::runtime_error("cannot move " + utf8(e->file) + ": " + ec.message());
        }
    }
    std::printf("%zu entries; %zu pieces of art appear in %zu palette variants\n", entries.size(), shared, variants);
    if (a.flag("apply")) {
        std::printf("wrote one palette-independent image per piece to shared/ and moved the variants to variants/\n");
    } else if (shared) {
        std::printf("run with --apply to keep one palette-independent image per piece (shared/<ti-|oi-key>.png)\n");
    }
    return 0;
}

// ---- preview ----

void blit(Image& dst, const Image& src, int x0, int y0, int w, int h) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int dx = x0 + x, dy = y0 + y;
            if (dx < 0 || dy < 0 || dx >= dst.width || dy >= dst.height) continue;
            const std::uint32_t c = src.row(static_cast<int>(static_cast<long long>(y) * src.height / h))
                                        [static_cast<long long>(x) * src.width / w];
            if ((c >> 24) >= 128) dst.row(dy)[dx] = c | 0xFF000000u;
        }
    }
}

int preview(const Args& a) {
    if (a.positional.size() != 1 || !a.value("o")) {
        throw std::runtime_error("usage: anygba hdpack preview DIR -o FILE.png [--pack PACK] [--scale N] [--kind K] [--limit N]");
    }
    const fs::path dir = path_arg(a.positional[0]);
    auto entries = entries_of(dir);
    const std::string kind = a.value("kind").value_or("all");
    std::erase_if(entries, [&](const Entry& e) {
        return kind != "all" && !(kind == "tile" && e.type == hd::KeyType::Tile) && !(kind == "obj" && e.type == hd::KeyType::Obj) &&
               !(kind == "bitmap" && e.type == hd::KeyType::Bitmap);
    });
    std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) { return x.frames > y.frames; });
    const std::size_t limit = static_cast<std::size_t>(to_int(a.value("limit").value_or("400"), "--limit", 1, 100000));
    if (entries.size() > limit) entries.resize(limit);
    const int scale = to_int(a.value("scale").value_or("4"), "--scale", 1, 16);
    std::unique_ptr<hd::Pack> pack;
    if (const auto p = a.value("pack")) pack = std::make_unique<hd::Pack>(path_arg(*p));

    // Each cell: the original at `scale` x, and with --pack its replacement beside it.
    int cell_w = 0, cell_h = 0;
    std::vector<Image> originals;
    for (const Entry& e : entries) {
        originals.push_back(to_image(load(e.file)));
        cell_w = std::max(cell_w, originals.back().width * scale);
        cell_h = std::max(cell_h, originals.back().height * scale);
    }
    if (entries.empty()) throw std::runtime_error("nothing to preview in " + utf8(dir));
    const int pair_w = (pack ? 2 * cell_w + 2 : cell_w) + 4;
    const int columns = std::max(1, std::min(static_cast<int>(entries.size()), 2048 / pair_w));
    const int rows = (static_cast<int>(entries.size()) + columns - 1) / columns;
    Image sheet;
    sheet.resize(columns * pair_w, rows * (cell_h + 4));
    std::fill(sheet.pixels.begin(), sheet.pixels.end(), 0xFF402020u);
    int replaced = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const int x = static_cast<int>(i % static_cast<std::size_t>(columns)) * pair_w + 2;
        const int y = static_cast<int>(i / static_cast<std::size_t>(columns)) * (cell_h + 4) + 2;
        const Image& o = originals[i];
        blit(sheet, o, x, y, o.width * scale, o.height * scale);
        if (!pack) continue;
        const hd::Texture* t = pack->find(entries[i].type, entries[i].key, o.width, o.height, scale);
        if (!t && !entries[i].indices.empty()) {
            if (const auto k = hd::parse_key(entries[i].indices)) t = pack->find(k->type, k->hash, o.width, o.height, scale);
        }
        if (!t) continue;
        ++replaced;
        blit(sheet, to_image(*t), x + cell_w + 2, y, o.width * scale, o.height * scale);
    }
    save(path_arg(*a.value("o")), sheet);
    std::printf("preview of %zu entries", entries.size());
    if (pack) std::printf(", %d with replacement art", replaced);
    std::printf(": %s (%dx%d)\n", a.value("o")->c_str(), sheet.width, sheet.height);
    for (const auto& w : pack ? pack->take_warnings() : std::vector<std::string>{}) std::fprintf(stderr, "warning: %s\n", w.c_str());
    return 0;
}

// ---- import ----

int import(const Args& a) {
    if (a.positional.size() != 2) throw std::runtime_error("usage: anygba hdpack import SRC PACK [--dump DUMP] [--title T]");
    const fs::path src = path_arg(a.positional[0]);
    const fs::path pack = path_arg(a.positional[1]);
    std::map<std::pair<int, std::uint64_t>, std::pair<int, int>> sizes;  // original sizes from the dump
    if (const auto d = a.value("dump")) {
        for (const Entry& e : entries_of(path_arg(*d))) {
            sizes[{static_cast<int>(e.type), e.key}] = {e.width, e.height};
            if (const auto k = hd::parse_key(e.indices)) sizes[{static_cast<int>(k->type), k->hash}] = {e.width, e.height};
        }
    }
    int imported = 0, skipped = 0;
    for (const Entry& e : entries_of(src)) {
        std::pair<int, int> original{8, 8};
        if (e.type != hd::KeyType::Tile && e.type != hd::KeyType::TileIndices) {
            const auto it = sizes.find({static_cast<int>(e.type), e.key});
            if (it == sizes.end()) {
                std::fprintf(stderr, "skipped %s: its original size is unknown (pass --dump)\n", utf8(e.file).c_str());
                ++skipped;
                continue;
            }
            original = it->second;
        }
        const hd::Texture t = load(e.file);
        const int s = t.width / original.first;
        if (t.width % original.first || t.height % original.second || t.height / original.second != s || s < 1 || s > 16) {
            std::fprintf(stderr, "skipped %s: %dx%d is not a 1x-16x multiple of %dx%d\n", utf8(e.file).c_str(), t.width,
                         t.height, original.first, original.second);
            ++skipped;
            continue;
        }
        const fs::path to = pack / hd::Dumper::folder(e.type) / (e.name + ".png");
        fs::create_directories(to.parent_path());
        fs::copy_file(e.file, to, fs::copy_options::overwrite_existing);
        ++imported;
    }
    std::error_code ec;
    if (!fs::exists(pack / "pack.toml", ec)) {
        std::ofstream out(pack / "pack.toml", std::ios::binary);
        out << "title = " << hd::json::quote(a.value("title").value_or(utf8(pack.filename()))) << "\nupscale = \"nearest\"\n";
    }
    std::printf("imported %d images into %s, skipped %d\n", imported, utf8(pack).c_str(), skipped);
    return skipped && !imported ? 1 : 0;
}

// ---- ai-upscale ----

int ai_upscale(const Args& a) {
    if (a.positional.size() != 2) {
        throw std::runtime_error(
            "usage: anygba hdpack ai-upscale DUMP PACK [--model general-x4v3|x4plus] [--device auto|dml|cuda|cpu] "
            "[--kinds tiles,objs,bitmaps] [--min-frames N] [--no-context] [--force] [--cache DIR]");
    }
    const fs::path dump = path_arg(a.positional[0]);
    const fs::path pack = path_arg(a.positional[1]);
    video::Options parsed;
    if (const auto m = a.value("model")) {
        if (const auto e = video::apply_setting(parsed, "ai-model", *m)) throw std::runtime_error(*e);
    } else {
        parsed.ai_model = video::AiModel::X4Plus;  // best quality; offline time is cheap
    }
    const auto device = hd::ai::parse_device(a.value("device").value_or("auto"));
    if (!device) throw std::runtime_error("--device must be auto, dml, cuda or cpu");
    const std::string kinds = a.value("kinds").value_or("tiles,objs,bitmaps");
    const auto wanted = [&](hd::KeyType t) {
        return (t == hd::KeyType::Tile && kinds.find("tiles") != std::string::npos) ||
               (t == hd::KeyType::Obj && kinds.find("objs") != std::string::npos) ||
               (t == hd::KeyType::Bitmap && kinds.find("bitmaps") != std::string::npos);
    };
    const std::uint64_t min_frames = static_cast<std::uint64_t>(to_int(a.value("min-frames").value_or("0"), "--min-frames", 0, 1 << 30));
    const fs::path cache = a.value("cache") ? path_arg(*a.value("cache")) : hd::ai::default_cache(data_dir());

    std::string error;
    std::printf("loading %s...\n", hd::ai::info(parsed.ai_model).name);
    auto up = hd::ai::Upscaler::create(parsed.ai_model, cache, *device, &error);
    if (!up) throw std::runtime_error(error);
    std::printf("%s on %s; weights: %s, %s\n", hd::ai::info(parsed.ai_model).name, up->device().c_str(),
                hd::ai::info(parsed.ai_model).source, hd::ai::info(parsed.ai_model).licence);

    const auto entries = entries_of(dump);
    const auto start = std::chrono::steady_clock::now();
    int done = 0, kept = 0, with_context = 0;
    const int f = up->factor();
    Image out;
    for (const Entry& e : entries) {
        if (!wanted(e.type) || e.frames < min_frames) continue;
        const fs::path to = pack / hd::Dumper::folder(e.type) / (e.name + ".png");
        std::error_code ec;
        if (!a.flag("force") && fs::exists(to, ec)) {
            ++kept;
            continue;
        }
        fs::path context = e.file;
        context.replace_extension(".context.png");
        if (e.type == hd::KeyType::Tile && !a.flag("no-context") && fs::exists(context, ec)) {
            // The tile with its neighbours as first seen (the tile at 8,8 of 24x24, unflipped):
            // the model sees across tile edges, so neighbouring tiles join without seams.
            up->upscale(to_image(load(context)), out);
            Image crop;
            crop.resize(8 * f, 8 * f);
            for (int y = 0; y < 8 * f; ++y) std::copy_n(out.row(8 * f + y) + 8 * f, 8 * f, crop.row(y));
            save(to, crop);
            ++with_context;
        } else {
            up->upscale(to_image(load(e.file)), out);
            save(to, out);
        }
        ++done;
        if (done % 250 == 0) std::printf("  %d images...\n", done);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::error_code ec;
    if (!fs::exists(pack / "pack.toml", ec)) {
        std::ofstream t(pack / "pack.toml", std::ios::binary);
        t << "title = " << hd::json::quote(std::string("AI ") + hd::ai::info(parsed.ai_model).name + " pack") << "\n"
          << "upscale = \"linear\"\n";
    }
    std::printf("upscaled %d images (%d with context) in %.1f s (%.1f per second) on %s; %d already there\n", done,
                with_context, seconds, seconds > 0 ? done / seconds : 0.0, up->device().c_str(), kept);
    return 0;
}

int fetch_runtime(const Args& a) {
    const fs::path cache = a.value("cache") ? path_arg(*a.value("cache")) : hd::ai::default_cache(data_dir());
    std::string error;
    const auto lib = hd::ai::fetch_runtime(cache, a.flag("directml"), &error);
    if (!lib) throw std::runtime_error(error);
    std::printf("ONNX Runtime: %s\n", utf8(*lib).c_str());
    return 0;
}

}  // namespace

std::string usage() {
    return "usage: anygba hdpack <command> [arguments]   (docs/user/HD-PACKS.md)\n"
           "\n"
           "  dedupe DUMP [--apply]            find art that differs only by palette; with --apply keep one\n"
           "                                   palette-independent image each (shared/) and move the variants\n"
           "  preview DIR -o FILE.png          contact sheet of a dump or pack, most used first\n"
           "      [--pack PACK] [--scale N] [--kind tile|obj|bitmap] [--limit N]\n"
           "                                   with --pack, each original beside its replacement\n"
           "  import SRC PACK [--dump DUMP]    check and copy edited art (key-named PNGs, 1x-16x) into a pack\n"
           "      [--title T]\n"
           "  ai-upscale DUMP PACK             generate a pack from a dump with an AI upscaler (ONNX Runtime)\n"
           "      [--model general-x4v3|x4plus] [--device auto|dml|cuda|cpu] [--kinds tiles,objs,bitmaps]\n"
           "      [--min-frames N] [--no-context] [--force] [--cache DIR]\n"
           "  fetch-runtime [--directml]       download the pinned ONNX Runtime (Windows x64, Linux x64;\n"
           "      [--cache DIR]                --directml also the DirectML redistributable for older Windows)\n";
}

int run(std::span<const std::string_view> args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help") {
        std::fputs(usage().c_str(), args.empty() ? stderr : stdout);
        return args.empty() ? 2 : 0;
    }
    const std::string_view cmd = args[0];
    const auto rest = args.subspan(1);
    try {
        if (cmd == "dedupe") return dedupe(parse(rest, {}));
        if (cmd == "preview") return preview(parse(rest, {"o", "pack", "scale", "kind", "limit"}));
        if (cmd == "import") return import(parse(rest, {"dump", "title"}));
        if (cmd == "ai-upscale") return ai_upscale(parse(rest, {"model", "device", "kinds", "min-frames", "cache"}));
        if (cmd == "fetch-runtime") return fetch_runtime(parse(rest, {"cache"}));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "anygba hdpack %.*s: %s\n", static_cast<int>(cmd.size()), cmd.data(), e.what());
        return 1;
    }
    std::fprintf(stderr, "anygba hdpack: unknown command '%.*s'\n\n%s", static_cast<int>(cmd.size()), cmd.data(), usage().c_str());
    return 2;
}

}  // namespace anygba::hdpack
