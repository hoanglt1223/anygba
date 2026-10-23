#include "config/game_config.h"

#define TOML_EXCEPTIONS 1
#include <toml.hpp>

#include <fstream>
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

}  // namespace

GameConfig parse_game_config(std::string_view text, std::string_view source_name) {
    toml::table root;
    try {
        root = toml::parse(text, source_name);
    } catch (const toml::parse_error& e) {
        std::ostringstream msg;
        msg << source_name << ":" << e.source().begin.line << ": " << e.description();
        throw ConfigError(msg.str());
    }

    const Reader r(source_name);
    r.only_keys(root, "the top level", {"game", "entry", "ram_region", "skip", "patch"});

    GameConfig config;
    if (const toml::node* game = root.get("game")) {
        const auto* table = game->as_table();
        if (!table) {
            r.fail(*game, "[game] must be a table");
        }
        r.only_keys(*table, "[game]", {"title"});
        if (const toml::node* title = table->get("title")) {
            if (!title->is_string()) {
                r.fail(*title, "game.title must be a string");
            }
            config.title = title->as_string()->get();
        }
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
    return config;
}

GameConfig load_game_config(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw ConfigError("cannot open config: " + path.string());
    }
    std::ostringstream text;
    text << file.rdbuf();
    return parse_game_config(text.str(), path.string());
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
    options.skip = config.skip;
    return options;
}

}  // namespace anygba::config
