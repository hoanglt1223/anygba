#include "video/hd/widescreen.h"

#include <toml.hpp>

#include <charconv>
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

std::optional<std::uint32_t> parse_number(std::string_view s) {
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    }
    std::uint32_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (ec != std::errc{} || end != s.data() + s.size() || s.empty()) return std::nullopt;
    return v;
}

std::uint8_t byte_at(const Memory& mem, std::uint32_t a) {
    switch (a >> 24) {
    case 0x00: return a < Memory::kBiosSize ? mem.bios[a] : 0;
    case 0x02: return mem.ewram[a & (Memory::kEwramSize - 1)];
    case 0x03: return mem.iwram[a & (Memory::kIwramSize - 1)];
    case 0x04: return (a & 0xFFFFFF) < Memory::kIoSize ? mem.io[a & 0x3FF] : 0;
    case 0x05: return mem.palette[a & (Memory::kPaletteSize - 1)];
    case 0x06: {
        std::uint32_t off = a & 0x1FFFF;
        if (off >= 0x18000) off -= 0x8000;
        return mem.vram[off];
    }
    case 0x07: return mem.oam[a & (Memory::kOamSize - 1)];
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        return mem.rom.empty() ? 0 : mem.rom[a & (Memory::kRomMax - 1)];
    default: return 0;
    }
}

void parse_layer_list(const toml::table& t, const char* key, Extend mode, std::array<Extend, 5>& layers,
                        const std::string& where) {
    const auto* arr = t[key].as_array();
    if (!arr) {
        if (t.contains(key)) throw std::runtime_error(where + ": '" + key + "' must be a list of layer names");
        return;
    }
    for (const auto& n : *arr) {
        const auto* s = n.as_string();
        const std::string name = s ? s->get() : std::string();
        int layer = -1;
        if (name.size() == 3 && name.starts_with("bg") && name[2] >= '0' && name[2] <= '3') layer = name[2] - '0';
        if (name == "obj") layer = 4;
        if (layer < 0) throw std::runtime_error(where + ": unknown layer '" + name + "' in '" + key + "' (bg0-bg3, obj)");
        if (layer == 4 && mode != Extend::Hide) {
            throw std::runtime_error(where + ": sprites can only be listed in 'hud'");
        }
        layers[static_cast<std::size_t>(layer)] = mode;
    }
}

SceneRule parse_scene(const toml::table& t, const std::string& where, bool is_default) {
    SceneRule r;
    r.name = t["name"].value_or(std::string(is_default ? "default" : ""));
    if (r.name.empty()) throw std::runtime_error(where + ": a scene needs a name");
    const std::string mode = t["mode"].value_or(std::string("widen"));
    if (mode != "widen" && mode != "pillarbox") throw std::runtime_error(where + ": mode must be \"widen\" or \"pillarbox\"");
    r.scene.pillarbox = mode == "pillarbox";
    parse_layer_list(t, "hud", Extend::Hide, r.scene.layers, where);
    parse_layer_list(t, "clamp", Extend::Clamp, r.scene.layers, where);
    parse_layer_list(t, "mirror", Extend::Mirror, r.scene.layers, where);
    if (const auto* when = t["when"].as_array()) {
        if (is_default) throw std::runtime_error(where + ": the default scene has no conditions");
        for (const auto& n : *when) {
            const auto* s = n.as_string();
            std::string error;
            const auto c = s ? Condition::parse(s->get(), &error) : std::nullopt;
            if (!c) throw std::runtime_error(where + ": " + (s ? error : std::string("conditions must be strings")));
            r.when.push_back(*c);
        }
    } else if (!is_default) {
        throw std::runtime_error(where + ": scene '" + r.name + "' needs a 'when' list");
    }
    r.provider = t["provider"].value_or(std::string());
    if (const auto* settings = t["settings"].as_table()) {
        for (const auto& [k, v] : *settings) {
            const auto i = v.value<std::int64_t>();
            if (!i || *i < 0 || *i > 0xFFFFFFFFll) {
                throw std::runtime_error(where + ": setting '" + std::string(k.str()) + "' must be a number");
            }
            r.provider_settings[std::string(k.str())] = static_cast<std::uint32_t>(*i);
        }
    }
    return r;
}

}  // namespace

std::uint32_t peek(const Memory& mem, std::uint32_t address, int size) {
    std::uint32_t v = 0;
    for (int i = 0; i < size; ++i) v |= static_cast<std::uint32_t>(byte_at(mem, address + static_cast<std::uint32_t>(i))) << (8 * i);
    return v;
}

std::optional<Condition> Condition::parse(std::string_view text, std::string* error) {
    const std::string original(text);
    std::vector<std::string_view> parts;
    while (!text.empty()) {
        while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
        const auto space = text.find(' ');
        if (!text.empty()) parts.push_back(text.substr(0, space));
        text = space == std::string_view::npos ? std::string_view{} : text.substr(space);
    }
    const auto bad = [&](const std::string& why) -> std::optional<Condition> {
        if (error) *error = "condition '" + original + "': " + why;
        return std::nullopt;
    };
    if (parts.size() != 4) return bad("expected \"u8|u16|u32 ADDRESS OP VALUE\"");
    Condition c;
    if (parts[0] == "u8") c.size = 1;
    else if (parts[0] == "u16") c.size = 2;
    else if (parts[0] == "u32") c.size = 4;
    else return bad("size must be u8, u16 or u32");
    const auto addr = parse_number(parts[1]);
    const auto value = parse_number(parts[3]);
    if (!addr || !value) return bad("address and value must be numbers");
    c.address = *addr;
    c.value = *value;
    const std::string_view op = parts[2];
    if (op == "==") c.op = Op::Eq;
    else if (op == "!=") c.op = Op::Ne;
    else if (op == "<") c.op = Op::Lt;
    else if (op == "<=") c.op = Op::Le;
    else if (op == ">") c.op = Op::Gt;
    else if (op == ">=") c.op = Op::Ge;
    else if (op == "&") c.op = Op::And;
    else return bad("operator must be one of == != < <= > >= &");
    return c;
}

bool Condition::holds(const Memory& mem) const {
    const std::uint32_t v = peek(mem, address, size);
    switch (op) {
    case Op::Eq: return v == value;
    case Op::Ne: return v != value;
    case Op::Lt: return v < value;
    case Op::Le: return v <= value;
    case Op::Gt: return v > value;
    case Op::Ge: return v >= value;
    case Op::And: return (v & value) != 0;
    }
    return false;
}

Rules Rules::parse(std::string_view text, const std::string& source) {
    toml::table t;
    try {
        t = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(source + ":" + std::to_string(e.source().begin.line) + ": " + std::string(e.description()));
    }
    Rules r;
    r.title_ = t["title"].value_or(std::string());
    if (const auto* d = t["default"].as_table()) {
        r.default_ = parse_scene(*d, source + " [default]", true);
    } else {
        r.default_.name = "default";
    }
    if (const auto* scenes = t["scene"].as_array()) {
        for (std::size_t i = 0; i < scenes->size(); ++i) {
            const auto* s = (*scenes)[i].as_table();
            const std::string where = source + " [[scene]] " + std::to_string(i + 1);
            if (!s) throw std::runtime_error(where + ": must be a table");
            r.scenes_.push_back(parse_scene(*s, where, false));
        }
    }
    for (const SceneRule* s : [&] {
             std::vector<const SceneRule*> all{&r.default_};
             for (const auto& x : r.scenes_) all.push_back(&x);
             return all;
         }()) {
        std::unique_ptr<MapProvider> p;
        if (!s->provider.empty()) {
            std::string error;
            p = make_provider(s->provider, s->provider_settings, &error);
            if (!p) throw std::runtime_error(source + ": scene '" + s->name + "': " + error);
        }
        r.providers_.push_back(std::move(p));
    }
    if (const auto* patches = t["patch"].as_array()) {
        for (const auto& n : *patches) {
            const auto* p = n.as_table();
            const auto addr = p ? (*p)["address"].value<std::int64_t>() : std::nullopt;
            const auto value = p ? (*p)["value"].value<std::int64_t>() : std::nullopt;
            if (!addr || !value || *addr < 0x08000000 || *addr > 0x09FFFFFE || (*addr & 1) || *value < 0 || *value > 0xFFFF) {
                throw std::runtime_error(source + ": each [[patch]] needs an even ROM address and a 16-bit value");
            }
            r.patches_[static_cast<std::uint32_t>(*addr)] = static_cast<std::uint16_t>(*value);
        }
    }
    return r;
}

std::optional<Rules> Rules::find(std::string_view game_code, const std::vector<fs::path>& dirs, fs::path* found) {
    for (const fs::path& dir : dirs) {
        const fs::path file = dir / (std::string(game_code) + ".toml");
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) continue;
        std::ifstream in(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (found) *found = file;
        return parse(text, utf8(file));
    }
    return std::nullopt;
}

const std::string& Rules::apply(const Memory& mem, Frame& frame, int extra) {
    for (auto& e : frame.extension) e = {};
    const SceneRule* chosen = &default_;
    std::size_t index = 0;
    for (std::size_t i = 0; i < scenes_.size(); ++i) {
        bool all = true;
        for (const Condition& c : scenes_[i].when) all = all && c.holds(mem);
        if (all) {
            chosen = &scenes_[i];
            index = i + 1;
            break;
        }
    }
    frame.scene = chosen->scene;
    if (MapProvider* p = providers_[index].get(); p && !chosen->scene.pillarbox) {
        if (!p->extend(mem, frame, extra)) {
            // The provider does not recognise what is on screen: never show wrong tiles.
            for (auto& e : frame.extension) e = {};
            frame.scene.pillarbox = true;
            fallback_name_ = chosen->name + " (pillarbox)";
            return fallback_name_;
        }
    }
    return chosen->name;
}

}  // namespace anygba::video::hd
