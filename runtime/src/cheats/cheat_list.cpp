#include "cheats/cheat_list.h"

#define TOML_EXCEPTIONS 1
#include <toml.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace anygba::cheats {

namespace fs = std::filesystem;

namespace {

class Reader {
public:
    explicit Reader(std::string source) : source_(std::move(source)) {}

    [[noreturn]] void fail(const toml::node& node, const std::string& message) const {
        throw std::runtime_error(source_ + ":" + std::to_string(node.source().begin.line) + ": " + message);
    }

    void only_keys(const toml::table& table, std::string_view context, std::initializer_list<std::string_view> keys) const {
        const std::set<std::string_view> allowed(keys);
        for (const auto& [key, node] : table) {
            if (!allowed.contains(key.str())) {
                fail(node, "unknown key '" + std::string(key.str()) + "' in " + std::string(context));
            }
        }
    }

    std::string string(const toml::table& table, std::string_view key, bool required) const {
        const toml::node* node = table.get(key);
        if (!node) {
            if (required) fail(table, "missing '" + std::string(key) + "'");
            return {};
        }
        const auto* s = node->as_string();
        if (!s) fail(*node, "'" + std::string(key) + "' must be a string");
        return s->get();
    }

    std::uint32_t u32(const toml::node& node, std::string_view what) const {
        const auto* v = node.as_integer();
        if (!v || v->get() < 0 || v->get() > std::numeric_limits<std::uint32_t>::max()) {
            fail(node, std::string(what) + " must be an integer in 0..0xFFFFFFFF");
        }
        return static_cast<std::uint32_t>(v->get());
    }

    std::vector<std::string> strings(const toml::table& table, std::string_view key) const {
        const toml::node* node = table.get(key);
        if (!node) fail(table, "missing '" + std::string(key) + "'");
        std::vector<std::string> out;
        if (const auto* s = node->as_string()) {
            out.push_back(s->get());
            return out;
        }
        const auto* array = node->as_array();
        if (!array) fail(*node, "'" + std::string(key) + "' must be a string or an array of strings");
        for (const auto& item : *array) {
            const auto* s = item.as_string();
            if (!s) fail(item, "'" + std::string(key) + "' entries must be strings");
            out.push_back(s->get());
        }
        return out;
    }

private:
    std::string source_;
};

toml::table parse(const fs::path& path) {
    try {
        return toml::parse_file(path.u8string());
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(path.string() + ":" + std::to_string(e.source().begin.line) + ": " +
                                 std::string(e.description()));
    }
}

// "25 Pikachu" or "0x19 Pikachu".
Choice parse_choice(const Reader& r, const toml::node& node) {
    const auto* s = node.as_string();
    if (!s) r.fail(node, "list entries must be strings like \"25 Pikachu\"");
    const std::string& text = s->get();
    const auto space = text.find(' ');
    const std::string number = text.substr(0, space);
    char* end = nullptr;
    const unsigned long v = std::strtoul(number.c_str(), &end, 0);
    if (number.empty() || *end != '\0' || space == std::string::npos || v > 0xFFFFFFFFul) {
        r.fail(node, "list entry '" + text + "' must be a number, a space and a name");
    }
    return {static_cast<std::uint32_t>(v), text.substr(space + 1)};
}

Format parse_format_or_fail(const Reader& r, const toml::table& t) {
    const std::string name = r.string(t, "format", false);
    if (name.empty()) return Format::Auto;
    const auto f = parse_format(name);
    if (!f) r.fail(t, "unknown format '" + name + "' (auto, raw, gsv1, gsv1raw, arv3, arv3raw, cb)");
    return *f;
}

std::vector<std::string> code_lines(const std::vector<std::string>& entries) {
    std::vector<std::string> lines;
    for (const auto& e : entries) {
        for (auto& l : split_lines(e)) lines.push_back(std::move(l));
    }
    return lines;
}

}  // namespace

std::string Param::describe(std::uint32_t value) const {
    for (const Choice& c : choices) {
        if (c.value == value) return c.name;
    }
    return std::to_string(value);
}

bool Param::accepts(std::uint32_t value) const {
    if (choices.empty()) return value >= min && value <= max;
    return std::any_of(choices.begin(), choices.end(), [value](const Choice& c) { return c.value == value; });
}

bool has_slot(std::string_view line) { return line.find("{v:") != std::string_view::npos; }

std::string expand(std::string_view line, std::uint32_t value) {
    std::string out;
    while (true) {
        const auto at = line.find("{v:");
        if (at == std::string_view::npos) {
            out += line;
            return out;
        }
        out += line.substr(0, at);
        const auto close = line.find('}', at);
        if (close == std::string_view::npos || close != at + 4 || line[at + 3] < '1' || line[at + 3] > '8') {
            throw CodeError("malformed value slot in '" + std::string(line) + "' (expected {v:1} to {v:8})");
        }
        const int digits = line[at + 3] - '0';
        const std::uint64_t limit = std::uint64_t{1} << (4 * digits);
        if (value >= limit) {
            throw CodeError("value " + std::to_string(value) + " does not fit in " + std::to_string(digits) + " hex digits");
        }
        char buf[16];
        std::snprintf(buf, sizeof buf, "%0*X", digits, value);
        out += buf;
        line.remove_prefix(close + 1);
    }
}

bool CheatList::load_database(const fs::path& path, std::uint32_t rom_crc) {
    const toml::table root = parse(path);
    const Reader r(path.string());
    r.only_keys(root, "a cheat database", {"game", "lists", "cheat"});
    if (const toml::node* game = root.get("game")) {
        const auto* t = game->as_table();
        if (!t) r.fail(*game, "[game] must be a table");
        r.only_keys(*t, "[game]", {"title", "code", "crc32", "source", "license"});
        if (const toml::node* crcs = t->get("crc32")) {
            const auto* array = crcs->as_array();
            if (!array) r.fail(*crcs, "crc32 must be an array of hex strings");
            bool match = array->empty();
            for (const auto& item : *array) {
                const auto* s = item.as_string();
                if (!s) r.fail(item, "crc32 entries must be hex strings");
                match = match || std::strtoul(s->get().c_str(), nullptr, 16) == rom_crc;
            }
            if (!match) return false;
        }
    }

    std::map<std::string, std::vector<Choice>> lists;
    if (const toml::node* node = root.get("lists")) {
        const auto* t = node->as_table();
        if (!t) r.fail(*node, "[lists] must be a table");
        for (const auto& [key, value] : *t) {
            const auto* array = value.as_array();
            if (!array) r.fail(value, "list '" + std::string(key.str()) + "' must be an array of strings");
            std::vector<Choice>& list = lists[std::string(key.str())];
            for (const auto& item : *array) list.push_back(parse_choice(r, item));
        }
    }

    std::vector<Cheat> added;
    if (const toml::node* node = root.get("cheat")) {
        const auto* array = node->as_array();
        if (!array) r.fail(*node, "cheats must be [[cheat]] tables");
        for (const auto& item : *array) {
            const auto* t = item.as_table();
            if (!t) r.fail(item, "cheats must be [[cheat]] tables");
            r.only_keys(*t, "[[cheat]]", {"name", "format", "code", "note", "param", "default"});
            Cheat c;
            c.name = r.string(*t, "name", true);
            c.format = parse_format_or_fail(r, *t);
            c.code = code_lines(r.strings(*t, "code"));
            c.note = r.string(*t, "note", false);
            const bool slotted = std::any_of(c.code.begin(), c.code.end(), [](const std::string& l) { return has_slot(l); });
            if (const toml::node* p = t->get("param")) {
                const auto* pt = p->as_table();
                if (!pt) r.fail(*p, "param must be a table");
                r.only_keys(*pt, "param", {"label", "list", "min", "max"});
                Param param;
                param.label = r.string(*pt, "label", false);
                if (param.label.empty()) param.label = "Value";
                const std::string list = r.string(*pt, "list", false);
                if (!list.empty()) {
                    const auto it = lists.find(list);
                    if (it == lists.end()) r.fail(*p, "unknown list '" + list + "'");
                    param.choices = it->second;
                } else {
                    const toml::node* lo = pt->get("min");
                    const toml::node* hi = pt->get("max");
                    if (!lo || !hi) r.fail(*p, "param needs a list or min and max");
                    param.min = r.u32(*lo, "min");
                    param.max = r.u32(*hi, "max");
                    if (param.min > param.max) r.fail(*p, "min is above max");
                }
                c.param = std::move(param);
                c.value = c.param->choices.empty() ? c.param->min : c.param->choices.front().value;
                if (const toml::node* d = t->get("default")) {
                    c.value = r.u32(*d, "default");
                    if (!c.param->accepts(c.value)) r.fail(*d, "default is not one of the choices");
                }
            }
            if (slotted != c.param.has_value()) {
                r.fail(*t, slotted ? "code has a {v:N} slot but no param" : "param given but the code has no {v:N} slot");
            }
            // Check the code now: a broken entry (community lists have some) is reported and
            // left out, and the rest of the file still loads.
            try {
                std::vector<std::string> lines;
                for (const auto& l : c.code) lines.push_back(expand(l, c.value));
                cheats::compile(c.format, lines);
            } catch (const CodeError& e) {
                warnings_.push_back(path.string() + ":" + std::to_string(t->source().begin.line) + ": cheat '" +
                                    c.name + "' skipped: " + e.what());
                continue;
            }
            added.push_back(std::move(c));
        }
    }
    for (auto& c : added) cheats_.push_back(std::move(c));
    ++generation_;
    return true;
}

void CheatList::load_user(const fs::path& path, bool persistent) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    const toml::table root = parse(path);
    const Reader r(path.string());
    r.only_keys(root, "a cheat file", {"cheat", "database"});
    if (const toml::node* node = root.get("cheat")) {
        const auto* array = node->as_array();
        if (!array) r.fail(*node, "cheats must be [[cheat]] tables");
        for (const auto& item : *array) {
            const auto* t = item.as_table();
            if (!t) r.fail(item, "cheats must be [[cheat]] tables");
            r.only_keys(*t, "[[cheat]]", {"name", "format", "code", "enabled", "note"});
            Cheat c;
            c.name = r.string(*t, "name", true);
            c.format = parse_format_or_fail(r, *t);
            c.code = code_lines(r.strings(*t, "code"));
            c.note = r.string(*t, "note", false);
            c.custom = true;
            c.persistent = persistent;
            bool enabled = true;
            if (const toml::node* e = t->get("enabled")) {
                const auto* b = e->as_boolean();
                if (!b) r.fail(*e, "enabled must be true or false");
                enabled = b->get();
            }
            cheats_.push_back(std::move(c));
            if (enabled) {
                if (auto error = set_enabled(cheats_.size() - 1, true)) {
                    warnings_.push_back(path.string() + ": cheat '" + cheats_.back().name + "': " + *error);
                }
            }
        }
    }
    if (const toml::node* node = root.get("database")) {
        const auto* t = node->as_table();
        if (!t) r.fail(*node, "[database] must be a table");
        for (const auto& [key, value] : *t) {
            const std::string name(key.str());
            const auto it = std::find_if(cheats_.begin(), cheats_.end(),
                                         [&](const Cheat& c) { return !c.custom && c.name == name; });
            if (it == cheats_.end()) {
                warnings_.push_back(path.string() + ": no cheat named '" + name + "' in the cheat database");
                continue;
            }
            const auto index = static_cast<std::size_t>(it - cheats_.begin());
            std::optional<std::string> error;
            if (const auto* b = value.as_boolean()) {
                if (it->param) r.fail(value, "'" + name + "' takes a value, not true/false");
                error = set_enabled(index, b->get());
            } else {
                if (!it->param) r.fail(value, "'" + name + "' takes true or false");
                error = set_value(index, r.u32(value, name));
            }
            if (error) warnings_.push_back(path.string() + ": cheat '" + name + "': " + *error);
        }
    }
}

std::optional<std::string> CheatList::save_user(const fs::path& path, std::string_view title) const {
    toml::array custom;
    toml::table picks;
    for (const Cheat& c : cheats_) {
        if (!c.persistent) continue;
        if (c.custom) {
            toml::array code;
            for (const auto& l : c.code) code.push_back(l);
            custom.push_back(toml::table{{"name", c.name},
                                         {"format", format_name(c.format)},
                                         {"code", std::move(code)},
                                         {"enabled", c.enabled}});
        } else if (c.enabled) {
            if (c.param) {
                picks.insert_or_assign(c.name, static_cast<std::int64_t>(c.value));
            } else {
                picks.insert_or_assign(c.name, true);
            }
        }
    }
    toml::table root;
    if (!custom.empty()) root.insert("cheat", std::move(custom));
    if (!picks.empty()) root.insert("database", std::move(picks));

    std::ostringstream text;
    text << "# AnyGBA cheats for " << title << ". Written by the game; edit while it is not running.\n"
         << "# Format: docs/user/CHEATS.md\n\n"
         << root << "\n";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text.str();
        out.close();
        if (!out) return "cannot write " + tmp.string();
    }
    fs::rename(tmp, path, ec);
    if (ec) return "cannot replace " + path.string() + ": " + ec.message();
    return std::nullopt;
}

std::size_t CheatList::add_custom(std::string name, Format format, std::vector<std::string> lines, bool persistent) {
    Cheat c;
    c.name = std::move(name);
    c.format = format;
    c.code = std::move(lines);
    c.custom = true;
    c.persistent = persistent;
    c.program = cheats::compile(format, c.code);  // throws CodeError
    c.format = c.program.format;  // remember what Auto resolved to
    c.enabled = true;
    cheats_.push_back(std::move(c));
    ++generation_;
    return cheats_.size() - 1;
}

void CheatList::remove(std::size_t index) {
    if (index < cheats_.size() && cheats_[index].custom) {
        cheats_.erase(cheats_.begin() + static_cast<std::ptrdiff_t>(index));
        ++generation_;
    }
}

std::optional<std::string> CheatList::compile(Cheat& c) {
    try {
        std::vector<std::string> lines;
        for (const auto& l : c.code) lines.push_back(c.param ? expand(l, c.value) : l);
        c.program = cheats::compile(c.format, lines);
        return std::nullopt;
    } catch (const CodeError& e) {
        return std::string(e.what());
    }
}

std::optional<std::string> CheatList::set_enabled(std::size_t index, bool enabled) {
    if (index >= cheats_.size()) return "no such cheat";
    Cheat& c = cheats_[index];
    if (enabled) {
        if (auto error = compile(c)) {
            c.enabled = false;
            return error;
        }
    }
    c.enabled = enabled;
    ++generation_;
    return std::nullopt;
}

std::optional<std::string> CheatList::set_value(std::size_t index, std::uint32_t value) {
    if (index >= cheats_.size() || !cheats_[index].param) return "this cheat takes no value";
    Cheat& c = cheats_[index];
    if (!c.param->accepts(value)) return "value " + std::to_string(value) + " is not one of the choices";
    const std::uint32_t old = c.value;
    c.value = value;
    if (auto error = set_enabled(index, true)) {
        c.value = old;
        return error;
    }
    return std::nullopt;
}

std::vector<const Program*> CheatList::active() const {
    std::vector<const Program*> out;
    for (const Cheat& c : cheats_) {
        if (c.enabled) out.push_back(&c.program);
    }
    return out;
}

}  // namespace anygba::cheats
