#include "video/settings.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <system_error>

namespace anygba::video {

namespace fs = std::filesystem;

namespace {

template <typename T>
void cycle(T& value, std::span<const T> all, int dir) {
    const auto n = static_cast<int>(all.size());
    int i = 0;
    while (i < n && all[static_cast<std::size_t>(i)] != value) ++i;
    value = all[static_cast<std::size_t>(((i + dir) % n + n) % n)];
}

std::string on_off(bool b) { return b ? "on" : "off"; }

}  // namespace

Options resolve_settings(const char* game_config, const std::optional<fs::path>& saved_file,
                         const std::string& command_line, std::string* warning) {
    Options options;
    if (game_config && *game_config) {
        if (const auto error = apply_serialized(options, game_config)) {
            throw std::runtime_error("the game's [video] config: " + *error);
        }
    }
    std::error_code ec;
    if (saved_file && fs::exists(*saved_file, ec)) {
        std::ifstream in(*saved_file, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        Options saved = options;
        if (!in.good() && !in.eof()) {
            if (warning) *warning = "cannot read " + saved_file->string();
        } else if (const auto error = apply_serialized(saved, text)) {
            if (warning) *warning = saved_file->string() + ": " + *error + "; ignored";
        } else {
            options = saved;
        }
    }
    if (const auto error = apply_serialized(options, command_line)) {
        throw std::runtime_error(*error);
    }
    return options;
}

std::optional<std::string> save_settings(const fs::path& file, const Options& options) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << serialize(options) << '\n';
        out.close();
        if (!out) return "cannot write " + tmp.string();
    }
    fs::rename(tmp, file, ec);
    if (ec) return "cannot replace " + file.string() + ": " + ec.message();
    return std::nullopt;
}

std::vector<MenuItem> menu_items(const Options& o) {
    char gamma[16];
    std::snprintf(gamma, sizeof gamma, "%.1f", o.gamma);
    return {
        {"Filter", std::string(name(o.scaler))},
        {"Screen effect", std::string(name(o.effect))},
        {"Ghosting", on_off(o.ghosting)},
        {"Colour", o.color == Color::Raw ? "raw" : "GBA LCD"},
        {"Gamma", gamma},
        {"Debanding", on_off(o.deband)},
        {"Resolution", std::string(name(o.output))},
        {"Fullscreen", on_off(o.fullscreen)},
        {"Integer scale", on_off(o.integer_scale)},
    };
}

void menu_step(Options& o, int item, int dir) {
    switch (item) {
    case 0: cycle(o.scaler, all_scalers(), dir); break;
    case 1: cycle(o.effect, all_effects(), dir); break;
    case 2: o.ghosting = !o.ghosting; break;
    case 3: cycle(o.color, all_colors(), dir); break;
    case 4: {
        // Tenths, wrapping from the top of the range to the bottom.
        const int steps = static_cast<int>(std::lround((kMaxGamma - kMinGamma) * 10)) + 1;
        const int i = static_cast<int>(std::lround((o.gamma - kMinGamma) * 10));
        o.gamma = kMinGamma + (((i + dir) % steps + steps) % steps) / 10.0;
        break;
    }
    case 5: o.deband = !o.deband; break;
    case 6: cycle(o.output, all_outputs(), dir); break;
    case 7: o.fullscreen = !o.fullscreen; break;
    case 8: o.integer_scale = !o.integer_scale; break;
    default: break;
    }
}

}  // namespace anygba::video
