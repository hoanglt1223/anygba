#include "config/game_config.h"

#include "anygba/video_options.h"

#define TOML_EXCEPTIONS 1
#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>

namespace anygba::config {

namespace {

using analysis::Mode;

class Reader {
public:
    explicit Reader(std::string_view source) : source_(source) {}

    [[noreturn]] void fail(const toml::node& node, const std::string& message) const {
        const auto& where = node.source().begin;
        throw ConfigError(std::string(source_) + ":" + std::to_string(where.line) + ": " + message);
    }

    void only_keys(const toml::table& table, std::string_view context, std::initializer_list<std::string_view> keys) const {
        const std::set<std::string_view> allowed(keys);
        for (const auto& [key, node] : table) {
            if (!allowed.contains(key.str())) {
                fail(node, "unknown key '" + std::string(key.str()) + "' in " + std::string(context));
            }
        }
    }

    const toml::node& require(const toml::table& table, std::string_view context, std::string_view key) const {
        const toml::node* node = table.get(key);
        if (!node) {
            fail(table, std::string(context) + " needs '" + std::string(key) + "'");
        }
        return *node;
    }

    std::uint32_t u32(const toml::table& table, std::string_view context, std::string_view key) const {
        const toml::node& node = require(table, context, key);
        const auto* value = node.as_integer();
        if (!value || value->get() < 0 || value->get() > std::numeric_limits<std::uint32_t>::max()) {
            fail(node, std::string(context) + "." + std::string(key) + " must be an integer in 0..0xFFFFFFFF");
        }
        return static_cast<std::uint32_t>(value->get());
    }

    Mode mode(const toml::table& table, std::string_view context) const {
        const toml::node& node = require(table, context, "mode");
        const auto* value = node.as_string();
        if (value && value->get() == "arm") {
            return Mode::Arm;
        }
        if (value && value->get() == "thumb") {
            return Mode::Thumb;
        }
        fail(node, std::string(context) + ".mode must be \"arm\" or \"thumb\"");
    }

    std::vector<std::uint8_t> hex_bytes(const toml::table& table, std::string_view context) const {
        const toml::node& node = require(table, context, "bytes");
        const auto* value = node.as_string();
        std::string text;
        if (value) {
            for (const char ch : value->get()) {
                if (ch != ' ' && ch != '_') {
                    text += ch;
                }
            }
        }
        if (text.empty() || text.size() % 2 != 0 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
            fail(node, std::string(context) + ".bytes must be a non-empty hex string, e.g. \"00 00 a0 e1\"");
        }
        std::vector<std::uint8_t> bytes;
        for (std::size_t i = 0; i < text.size(); i += 2) {
            bytes.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
        }
        return bytes;
    }

    template <typename Fn>
    void each_table(const toml::table& root, std::string_view key, Fn&& fn) const {
        const toml::node* node = root.get(key);
        if (!node) {
            return;
        }
        const auto* array = node->as_array();
        if (!array || !array->is_array_of_tables()) {
            fail(*node, "'" + std::string(key) + "' must be an array of tables ([[" + std::string(key) + "]])");
        }
        for (const auto& item : *array) {
            fn(*item.as_table());
        }
    }

private:
    std::string_view source_;
};

// [video]: the game's default video settings, checked here so a typo fails the recompile.
std::string parse_video(const Reader& r, const toml::node& node) {
    const auto* table = node.as_table();
    if (!table) {
        r.fail(node, "[video] must be a table");
    }
    video::Options options;
    for (const auto& [key, value] : *table) {
        std::string text;
        if (const auto* s = value.as_string()) {
            text = s->get();
        } else if (const auto* b = value.as_boolean()) {
            text = b->get() ? "true" : "false";
        } else if (const auto* f = value.as_floating_point()) {
            text = std::to_string(f->get());
        } else if (const auto* i = value.as_integer()) {
            text = std::to_string(i->get());
        }
        if (const auto error = video::apply_setting(options, key.str(), text)) {
            r.fail(value, "video." + std::string(key.str()) + ": " + *error);
        }
    }
    return video::serialize(options);
}

bool is_identifier(const std::string& s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    return std::all_of(s.begin(), s.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; });
}

toml::table parse_toml(std::string_view text, std::string_view source_name) {
    try {
        return toml::parse(text, source_name);
    } catch (const toml::parse_error& e) {
        std::ostringstream msg;
        msg << source_name << ":" << e.source().begin.line << ": " << e.description();
        throw ConfigError(msg.str());
    }
}

std::string read_text(const std::filesystem::path& path, const char* what) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw ConfigError(std::string("cannot open ") + what + ": " + path.string());
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// [functions]: name = { addr = 0x08008BA0, mode = "thumb" }
FunctionTable parse_functions(const Reader& r, const toml::table& root) {
    FunctionTable functions;
    const toml::node* node = root.get("functions");
    if (!node) return functions;
    const auto* table = node->as_table();
    if (!table) r.fail(*node, "[functions] must be a table of name = { addr = ..., mode = ... }");
    for (const auto& [key, value] : *table) {
        const std::string name(key.str());
        const auto* entry = value.as_table();
        if (!entry || !is_identifier(name)) {
            r.fail(value, "functions." + name + " must be an identifier = { addr = ..., mode = \"arm\" | \"thumb\" }");
        }
        const std::string context = "functions." + name;
        r.only_keys(*entry, context, {"addr", "mode"});
        functions[name] = {r.u32(*entry, context, "addr"), r.mode(*entry, context)};
    }
    return functions;
}

void parse_overrides(const Reader& r, const toml::table& root, std::string_view source_name, std::vector<Override>& out) {
    r.each_table(root, "override", [&](const toml::table& t) {
        r.only_keys(t, "[[override]]", {"addr", "mode", "symbol", "impl", "enabled", "engine"});
        Override o;
        o.where = std::string(source_name) + ":" + std::to_string(t.source().begin.line);
        const bool has_addr = t.contains("addr");
        if (has_addr == t.contains("symbol")) {
            r.fail(t, "[[override]] needs exactly one of 'addr' (with 'mode') and 'symbol'");
        }
        if (has_addr) {
            o.addr = r.u32(t, "override", "addr");
            o.mode = r.mode(t, "override");
        } else {
            if (const toml::node* mode = t.get("mode")) {
                r.fail(*mode, "override.mode goes with addr; a symbol's mode comes from [functions]");
            }
            const toml::node& sym = *t.get("symbol");
            if (!sym.is_string() || !is_identifier(sym.as_string()->get())) {
                r.fail(sym, "override.symbol must be a function name from a [functions] table");
            }
            o.symbol = sym.as_string()->get();
        }
        const toml::node& impl = r.require(t, "override", "impl");
        if (!impl.is_string() || !is_identifier(impl.as_string()->get())) {
            r.fail(impl, "override.impl must be the C++ identifier given to ANYGBA_OVERRIDE");
        }
        o.impl = impl.as_string()->get();
        if (const toml::node* enabled = t.get("enabled")) {
            if (!enabled->is_boolean()) r.fail(*enabled, "override.enabled must be true or false");
            o.enabled = enabled->as_boolean()->get();
        }
        if (const toml::node* engine = t.get("engine")) {
            if (!engine->is_string() || engine->as_string()->get().empty()) {
                r.fail(*engine, "override.engine must be a non-empty string");
            }
            o.engine = engine->as_string()->get();
        }
        out.push_back(std::move(o));
    });
}

std::vector<std::filesystem::path> module_sources(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> sources;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
        const auto ext = item.path().extension();
        if (item.is_regular_file() && (ext == ".c" || ext == ".cpp")) {
            sources.push_back(std::filesystem::absolute(item.path()));
        }
    }
    if (ec) throw ConfigError("cannot list override module " + dir.string() + ": " + ec.message());
    std::sort(sources.begin(), sources.end());
    return sources;
}

}  // namespace

GameConfig parse_game_config(std::string_view text, std::string_view source_name) {
    const toml::table root = parse_toml(text, source_name);
    const Reader r(source_name);
    // [symbols] is for the runtime's agent control (docs/dev/GAME-CONFIG.md); the game checks it.
    r.only_keys(root, "the top level", {"game", "entry", "ram_region", "skip", "patch", "replace", "video", "symbols",
                                         "functions", "override"});

    GameConfig config;
    if (const toml::node* game = root.get("game")) {
        const auto* table = game->as_table();
        if (!table) {
            r.fail(*game, "[game] must be a table");
        }
        r.only_keys(*table, "[game]", {"title", "save", "rtc", "sources"});
        if (const toml::node* title = table->get("title")) {
            if (!title->is_string()) {
                r.fail(*title, "game.title must be a string");
            }
            config.title = title->as_string()->get();
        }
        if (const toml::node* save = table->get("save")) {
            static constexpr std::string_view kTypes[] = {"none", "sram", "flash64", "flash128",
                                                          "eeprom", "eeprom512", "eeprom8k"};
            const auto* value = save->as_string();
            if (!value || std::find(std::begin(kTypes), std::end(kTypes), value->get()) == std::end(kTypes)) {
                r.fail(*save, "game.save must be one of none, sram, flash64, flash128, eeprom, eeprom512, eeprom8k");
            }
            config.save_type = value->get();
        }
        if (const toml::node* rtc = table->get("rtc")) {
            if (!rtc->is_boolean()) {
                r.fail(*rtc, "game.rtc must be true or false");
            }
            config.rtc = rtc->as_boolean()->get() ? 1 : 0;
        }
        if (const toml::node* sources = table->get("sources")) {
            const auto* array = sources->as_array();
            if (!array) {
                r.fail(*sources, "game.sources must be an array of file names");
            }
            for (const auto& item : *array) {
                const auto* name = item.as_string();
                if (!name || !(name->get().ends_with(".c") || name->get().ends_with(".cpp"))) {
                    r.fail(item, "game.sources entries must be .c or .cpp file names");
                }
                config.sources.emplace_back(name->get());
            }
        }
    }
    if (const toml::node* video = root.get("video")) {
        config.video = parse_video(r, *video);
    }
    r.each_table(root, "entry", [&](const toml::table& t) {
        r.only_keys(t, "[[entry]]", {"addr", "mode"});
        config.entries.push_back({r.u32(t, "entry", "addr"), r.mode(t, "entry"), analysis::EntrySource::Config});
    });
    r.each_table(root, "ram_region", [&](const toml::table& t) {
        r.only_keys(t, "[[ram_region]]", {"rom_src", "ram_dst", "size", "mode"});
        RamCode code;
        code.region = {r.u32(t, "ram_region", "rom_src"), r.u32(t, "ram_region", "ram_dst"), r.u32(t, "ram_region", "size")};
        code.mode = r.mode(t, "ram_region");
        config.ram_code.push_back(code);
    });
    r.each_table(root, "skip", [&](const toml::table& t) {
        r.only_keys(t, "[[skip]]", {"start", "end"});
        const analysis::AddressRange range{r.u32(t, "skip", "start"), r.u32(t, "skip", "end")};
        if (range.end <= range.begin) {
            r.fail(t, "skip.end must be greater than skip.start");
        }
        config.skip.push_back(range);
    });
    r.each_table(root, "patch", [&](const toml::table& t) {
        r.only_keys(t, "[[patch]]", {"addr", "bytes"});
        config.patches.push_back({r.u32(t, "patch", "addr"), r.hex_bytes(t, "patch")});
    });
    r.each_table(root, "replace", [&](const toml::table& t) {
        r.only_keys(t, "[[replace]]", {"addr", "mode", "function"});
        Replacement rep{r.u32(t, "replace", "addr"), r.mode(t, "replace"), {}};
        const toml::node& fn = r.require(t, "replace", "function");
        const auto* name = fn.as_string();
        if (!name || !is_identifier(name->get()) || name->get().starts_with("f_") || name->get().starts_with("ag_") ||
            name->get().starts_with("anygba_")) {
            r.fail(fn, "replace.function must be a C identifier not starting with f_, ag_ or anygba_");
        }
        rep.function = name->get();
        config.replacements.push_back(rep);
    });
    if (!config.replacements.empty() && config.sources.empty()) {
        throw ConfigError(std::string(source_name) + ": [[replace]] needs game.sources naming the C file(s) that define the functions");
    }
    config.functions = parse_functions(r, root);
    parse_overrides(r, root, source_name, config.overrides);
    return config;
}

GameConfig load_game_config(const std::filesystem::path& path) {
    GameConfig config = parse_game_config(read_text(path, "config"), path.string());
    for (auto& source : config.sources) {
        source = std::filesystem::absolute(path.parent_path() / source);
        if (!std::filesystem::is_regular_file(source)) {
            throw ConfigError(path.string() + ": game.sources: no such file " + source.string());
        }
    }
    return config;
}

analysis::DiscoveryOptions apply_game_config(const GameConfig& config, analysis::Image& image) {
    analysis::DiscoveryOptions options;
    for (const auto& code : config.ram_code) {
        try {
            image.add_region(code.region);
        } catch (const std::invalid_argument& e) {
            throw ConfigError(std::string("ram_region: ") + e.what());
        }
        options.entries.push_back({code.region.ram_dst, code.mode, analysis::EntrySource::Config});
    }
    for (const auto& patch : config.patches) {
        try {
            image.patch(patch.addr, patch.bytes);
        } catch (const std::invalid_argument& e) {
            throw ConfigError(std::string("patch: ") + e.what());
        }
    }
    options.entries.insert(options.entries.end(), config.entries.begin(), config.entries.end());
    for (const auto& rep : config.replacements) {
        options.entries.push_back({rep.addr, rep.mode, analysis::EntrySource::Config});
    }
    options.skip = config.skip;
    return options;
}

OverrideModule parse_override_module(std::string_view text, std::string_view source_name) {
    const toml::table root = parse_toml(text, source_name);
    const Reader r(source_name);
    r.only_keys(root, "the top level of an override module", {"override"});
    OverrideModule module;
    parse_overrides(r, root, source_name, module.overrides);
    if (module.overrides.empty()) {
        throw ConfigError(std::string(source_name) + ": an override module needs at least one [[override]]");
    }
    return module;
}

OverrideModule load_override_module(const std::filesystem::path& dir) {
    const std::filesystem::path file = dir / "overrides.toml";
    if (!std::filesystem::is_regular_file(file)) {
        throw ConfigError("override module " + dir.string() + " has no overrides.toml");
    }
    OverrideModule module = parse_override_module(read_text(file, "override module"), file.string());
    module.dir = std::filesystem::absolute(dir);
    module.sources = module_sources(dir);
    if (module.sources.empty()) {
        throw ConfigError("override module " + dir.string() + " has no .cpp or .c sources");
    }
    return module;
}

FunctionTable parse_function_symbols(std::string_view text, std::string_view source_name) {
    const toml::table root = parse_toml(text, source_name);
    return parse_functions(Reader(source_name), root);
}

FunctionTable load_function_symbols(const std::filesystem::path& path) {
    return parse_function_symbols(read_text(path, "symbols file"), path.string());
}

}  // namespace anygba::config
