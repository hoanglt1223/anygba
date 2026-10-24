#include "anygba/video_options.h"

#include <array>
#include <charconv>
#include <cstdio>

namespace anygba::video {

namespace {

constexpr std::array kScalers = {
    Scaler::Nearest, Scaler::Bilinear, Scaler::SharpBilinear, Scaler::Scale2x, Scaler::Scale3x, Scaler::Scale4x,
    Scaler::Eagle,   Scaler::Sai2x,    Scaler::SuperEagle,    Scaler::Super2xSai, Scaler::Hq2x, Scaler::Hq3x,
    Scaler::Hq4x,    Scaler::Xbr2x,    Scaler::Xbr3x,         Scaler::Xbr4x,   Scaler::Xbrz2x, Scaler::Xbrz3x,
    Scaler::Xbrz4x,  Scaler::Xbrz5x,   Scaler::Xbrz6x,
};
constexpr std::array kEffects = {Effect::None, Effect::LcdGrid, Effect::Scanlines, Effect::Crt};
constexpr std::array kColors = {Color::Raw, Color::GbaLcd};
constexpr std::array kOutputs = {Output::Window, Output::P720, Output::P1080, Output::P1440, Output::P2160,
                                 Output::Desktop};
constexpr std::array kAiModes = {Ai::Off, Ai::Frame, Ai::Layers};
constexpr std::array kAiModels = {AiModel::GeneralX4v3, AiModel::X4Plus};
constexpr std::array kWidescreens = {Widescreen::Off, Widescreen::W16x10, Widescreen::W16x9};

template <typename T, std::size_t N>
std::optional<T> find(const std::array<T, N>& all, std::string_view text) {
    for (T v : all) {
        if (name(v) == text) return v;
    }
    return std::nullopt;
}

template <typename T, std::size_t N>
std::string names(const std::array<T, N>& all) {
    std::string out;
    for (T v : all) {
        if (!out.empty()) out += ", ";
        out += name(v);
    }
    return out;
}

std::optional<bool> parse_bool(std::string_view v) {
    if (v == "true" || v == "on" || v == "1") return true;
    if (v == "false" || v == "off" || v == "0") return false;
    return std::nullopt;
}

std::string format_gamma(double g) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.2f", g);
    std::string s = buf;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}

}  // namespace

std::span<const Scaler> all_scalers() { return kScalers; }
std::span<const Effect> all_effects() { return kEffects; }
std::span<const Color> all_colors() { return kColors; }
std::span<const Output> all_outputs() { return kOutputs; }
std::span<const Ai> all_ai_modes() { return kAiModes; }
std::span<const AiModel> all_ai_models() { return kAiModels; }
std::span<const Widescreen> all_widescreens() { return kWidescreens; }

std::string_view name(Ai a) {
    switch (a) {
    case Ai::Off: return "off";
    case Ai::Frame: return "frame";
    case Ai::Layers: return "layers";
    }
    return "?";
}

std::string_view name(AiModel m) { return m == AiModel::GeneralX4v3 ? "general-x4v3" : "x4plus"; }

std::string_view name(Widescreen w) {
    switch (w) {
    case Widescreen::Off: return "off";
    case Widescreen::W16x10: return "16:10";
    case Widescreen::W16x9: return "16:9";
    }
    return "?";
}

int widescreen_extra(Widescreen w) {
    // 160 lines at 16:10 are 256 columns, at 16:9 284 (284.4 rounded down to an even width).
    switch (w) {
    case Widescreen::W16x10: return 8;
    case Widescreen::W16x9: return 22;
    default: return 0;
    }
}

std::string_view name(Scaler s) {
    switch (s) {
    case Scaler::Nearest: return "nearest";
    case Scaler::Bilinear: return "bilinear";
    case Scaler::SharpBilinear: return "sharp-bilinear";
    case Scaler::Scale2x: return "scale2x";
    case Scaler::Scale3x: return "scale3x";
    case Scaler::Scale4x: return "scale4x";
    case Scaler::Eagle: return "eagle";
    case Scaler::Sai2x: return "2xsai";
    case Scaler::SuperEagle: return "super-eagle";
    case Scaler::Super2xSai: return "super-2xsai";
    case Scaler::Hq2x: return "hq2x";
    case Scaler::Hq3x: return "hq3x";
    case Scaler::Hq4x: return "hq4x";
    case Scaler::Xbr2x: return "xbr2x";
    case Scaler::Xbr3x: return "xbr3x";
    case Scaler::Xbr4x: return "xbr4x";
    case Scaler::Xbrz2x: return "xbrz2x";
    case Scaler::Xbrz3x: return "xbrz3x";
    case Scaler::Xbrz4x: return "xbrz4x";
    case Scaler::Xbrz5x: return "xbrz5x";
    case Scaler::Xbrz6x: return "xbrz6x";
    }
    return "?";
}

std::string_view name(Effect e) {
    switch (e) {
    case Effect::None: return "none";
    case Effect::LcdGrid: return "lcd-grid";
    case Effect::Scanlines: return "scanlines";
    case Effect::Crt: return "crt";
    }
    return "?";
}

std::string_view name(Color c) { return c == Color::Raw ? "raw" : "gba-lcd"; }

std::string_view name(Output o) {
    switch (o) {
    case Output::Window: return "window";
    case Output::P720: return "720p";
    case Output::P1080: return "1080p";
    case Output::P1440: return "1440p";
    case Output::P2160: return "4k";
    case Output::Desktop: return "desktop";
    }
    return "?";
}

int factor(Scaler s) {
    switch (s) {
    case Scaler::Nearest:
    case Scaler::Bilinear:
    case Scaler::SharpBilinear: return 1;
    case Scaler::Scale3x:
    case Scaler::Hq3x:
    case Scaler::Xbr3x:
    case Scaler::Xbrz3x: return 3;
    case Scaler::Scale4x:
    case Scaler::Hq4x:
    case Scaler::Xbr4x:
    case Scaler::Xbrz4x: return 4;
    case Scaler::Xbrz5x: return 5;
    case Scaler::Xbrz6x: return 6;
    default: return 2;
    }
}

std::optional<std::pair<int, int>> output_size(Output o) {
    switch (o) {
    case Output::P720: return std::pair{1280, 720};
    case Output::P1080: return std::pair{1920, 1080};
    case Output::P1440: return std::pair{2560, 1440};
    case Output::P2160: return std::pair{3840, 2160};
    default: return std::nullopt;
    }
}

std::optional<std::string> parse_filter(std::string_view spec, Options& options) {
    if (spec.empty()) return "empty filter";
    std::optional<Scaler> scaler;
    std::optional<Effect> effect;
    bool ghosting = false;
    while (!spec.empty()) {
        const auto plus = spec.find('+');
        const std::string_view part = spec.substr(0, plus);
        spec = plus == std::string_view::npos ? std::string_view{} : spec.substr(plus + 1);
        if (part == "ghosting") {
            ghosting = true;
        } else if (const auto s = find(kScalers, part)) {
            if (scaler) return "more than one scaler in the filter";
            scaler = s;
        } else if (const auto e = find(kEffects, part)) {
            if (effect) return "more than one screen effect in the filter";
            effect = e;
        } else {
            return "unknown filter '" + std::string(part) + "' (scalers: " + names(kScalers) +
                   "; effects: " + names(kEffects) + ", ghosting)";
        }
    }
    options.scaler = scaler.value_or(Scaler::Nearest);
    options.effect = effect.value_or(Effect::None);
    options.ghosting = ghosting;
    return std::nullopt;
}

std::string filter_spec(const Options& options) {
    std::string s(name(options.scaler));
    if (options.effect != Effect::None) s += "+" + std::string(name(options.effect));
    if (options.ghosting) s += "+ghosting";
    return s;
}

std::optional<std::string> apply_setting(Options& options, std::string_view key, std::string_view value) {
    if (key == "filter") return parse_filter(value, options);
    if (key == "color") {
        const auto c = find(kColors, value);
        if (!c) return "color must be one of " + names(kColors);
        options.color = *c;
        return std::nullopt;
    }
    if (key == "output") {
        const auto o = find(kOutputs, value);
        if (!o) return "output must be one of " + names(kOutputs);
        options.output = *o;
        return std::nullopt;
    }
    if (key == "gamma") {
        double g = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), g);
        if (ec != std::errc{} || end != value.data() + value.size() || !(g >= kMinGamma && g <= kMaxGamma)) {
            return "gamma must be a number from 0.5 to 2.5";
        }
        options.gamma = g;
        return std::nullopt;
    }
    if (key == "internal-scale") {
        int n = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (ec != std::errc{} || end != value.data() + value.size() || n < 1 || n > kMaxInternalScale) {
            return "internal-scale must be a whole number from 1 to " + std::to_string(kMaxInternalScale);
        }
        options.internal_scale = n;
        return std::nullopt;
    }
    if (key == "layer-filter") {
        const auto s = find(kScalers, value);
        if (!s) return "layer-filter must be one of " + names(kScalers);
        options.layer_filter = *s;
        return std::nullopt;
    }
    if (key == "ai") {
        const auto a = find(kAiModes, value);
        if (!a) return "ai must be one of " + names(kAiModes);
        options.ai = *a;
        return std::nullopt;
    }
    if (key == "ai-model") {
        const auto m = find(kAiModels, value);
        if (!m) return "ai-model must be one of " + names(kAiModels);
        options.ai_model = *m;
        return std::nullopt;
    }
    if (key == "widescreen") {
        const auto w = find(kWidescreens, value);
        if (!w) return "widescreen must be one of " + names(kWidescreens);
        options.widescreen = *w;
        return std::nullopt;
    }
    bool* flag = key == "deband"          ? &options.deband
                 : key == "fullscreen"    ? &options.fullscreen
                 : key == "integer-scale" ? &options.integer_scale
                 : key == "smooth-affine" ? &options.smooth_affine
                 : key == "hd-pack"       ? &options.hd_pack
                 : key == "hd-dump"       ? &options.hd_dump
                                          : nullptr;
    if (!flag) {
        return "unknown video setting '" + std::string(key) +
               "' (filter, color, gamma, deband, output, fullscreen, integer-scale, internal-scale, layer-filter, "
               "smooth-affine, hd-pack, hd-dump, ai, ai-model, widescreen)";
    }
    const auto b = parse_bool(value);
    if (!b) return std::string(key) + " must be true or false";
    *flag = *b;
    return std::nullopt;
}

std::string serialize(const Options& o) {
    const Options d;
    std::string out;
    const auto add = [&](std::string_view key, const std::string& value) {
        if (!out.empty()) out += ';';
        out += std::string(key) + "=" + value;
    };
    if (o.scaler != d.scaler || o.effect != d.effect || o.ghosting != d.ghosting) add("filter", filter_spec(o));
    if (o.color != d.color) add("color", std::string(name(o.color)));
    if (o.gamma != d.gamma) add("gamma", format_gamma(o.gamma));
    if (o.deband != d.deband) add("deband", o.deband ? "true" : "false");
    if (o.output != d.output) add("output", std::string(name(o.output)));
    if (o.fullscreen != d.fullscreen) add("fullscreen", o.fullscreen ? "true" : "false");
    if (o.integer_scale != d.integer_scale) add("integer-scale", o.integer_scale ? "true" : "false");
    if (o.internal_scale != d.internal_scale) add("internal-scale", std::to_string(o.internal_scale));
    if (o.layer_filter != d.layer_filter) add("layer-filter", std::string(name(o.layer_filter)));
    if (o.smooth_affine != d.smooth_affine) add("smooth-affine", o.smooth_affine ? "true" : "false");
    if (o.hd_pack != d.hd_pack) add("hd-pack", o.hd_pack ? "true" : "false");
    if (o.hd_dump != d.hd_dump) add("hd-dump", o.hd_dump ? "true" : "false");
    if (o.ai != d.ai) add("ai", std::string(name(o.ai)));
    if (o.ai_model != d.ai_model) add("ai-model", std::string(name(o.ai_model)));
    if (o.widescreen != d.widescreen) add("widescreen", std::string(name(o.widescreen)));
    return out;
}

std::optional<std::string> apply_serialized(Options& options, std::string_view text) {
    while (!text.empty()) {
        const auto semi = text.find(';');
        const std::string_view item = text.substr(0, semi);
        text = semi == std::string_view::npos ? std::string_view{} : text.substr(semi + 1);
        const auto eq = item.find('=');
        if (eq == std::string_view::npos) return "video setting without '=': " + std::string(item);
        if (auto error = apply_setting(options, item.substr(0, eq), item.substr(eq + 1))) return error;
    }
    return std::nullopt;
}

}  // namespace anygba::video
