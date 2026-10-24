#include "control/symbols.h"

#define TOML_EXCEPTIONS 1
#include <toml.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace anygba::control {

namespace {

constexpr std::uint32_t kMaxBytes = 64 * 1024;  // one read, so a typo cannot dump all memory

struct TypeInfo {
    std::string_view name;
    SymType type;
    std::uint32_t size;  // 0: from `size`
};
constexpr TypeInfo kTypes[] = {
    {"u8", SymType::U8, 1},   {"u16", SymType::U16, 2},   {"u32", SymType::U32, 4},   {"s8", SymType::S8, 1},
    {"s16", SymType::S16, 2}, {"s32", SymType::S32, 4},   {"bool", SymType::Bool, 1}, {"bytes", SymType::Bytes, 0},
    {"text", SymType::Text, 0}, {"struct", SymType::Struct, 0},
};

std::string_view type_name(SymType t) {
    for (const auto& info : kTypes) {
        if (info.type == t) return info.name;
    }
    return "?";
}

bool scalar(SymType t) { return t != SymType::Bytes && t != SymType::Text && t != SymType::Struct; }

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

class Reader {
public:
    explicit Reader(std::string_view source) : source_(source) {}

    [[noreturn]] void fail(const toml::node& node, const std::string& what) const {
        std::ostringstream msg;
        msg << source_ << ":" << node.source().begin.line << ": " << what;
        throw std::runtime_error(msg.str());
    }

    std::uint32_t u32(const toml::table& t, std::string_view key, const std::string& ctx) const {
        const toml::node* n = t.get(key);
        const auto* v = n ? n->as_integer() : nullptr;
        if (!v || v->get() < 0 || v->get() > 0xFFFFFFFFll) fail(n ? *n : static_cast<const toml::node&>(t), ctx + "." + std::string(key) + " must be an integer from 0 to 0xFFFFFFFF");
        return static_cast<std::uint32_t>(v->get());
    }

    std::string str(const toml::table& t, std::string_view key, const std::string& ctx) const {
        const toml::node* n = t.get(key);
        if (!n) return {};
        const auto* v = n->as_string();
        if (!v) fail(*n, ctx + "." + std::string(key) + " must be a string");
        return v->get();
    }

    // The keys every symbol and field share: type, size, count, bit, xor, charset.
    void common(const toml::table& t, const std::string& ctx, SymField& f, bool allow_struct) const {
        const std::string type = str(t, "type", ctx);
        const auto it = std::find_if(std::begin(kTypes), std::end(kTypes), [&](const TypeInfo& i) { return i.name == type; });
        if (it == std::end(kTypes) || (!allow_struct && it->type == SymType::Struct)) {
            fail(t, ctx + ".type must be one of u8, u16, u32, s8, s16, s32, bool, bytes, text" +
                        (allow_struct ? std::string(", struct") : std::string()));
        }
        f.type = it->type;
        f.size = it->size;
        if (t.get("size")) {
            const std::uint32_t size = u32(t, "size", ctx);
            if (it->size != 0 && size != it->size) fail(t, ctx + ".size does not match type " + type);
            f.size = size;
        }
        if (f.size == 0 || f.size > kMaxBytes) fail(t, ctx + " needs a size from 1 to 65536 for type " + type);
        if (t.get("count")) f.count = u32(t, "count", ctx);
        if (f.count == 0 || static_cast<std::uint64_t>(f.count) * f.size > kMaxBytes) {
            fail(t, ctx + ".count * size must be from 1 to 65536 bytes");
        }
        if (t.get("bit")) {
            const std::uint32_t bit = u32(t, "bit", ctx);
            if (!scalar(f.type) || bit >= f.size * 8) fail(t, ctx + ".bit must name a bit of an integer type");
            f.bit = static_cast<int>(bit);
        }
        f.xor_with = str(t, "xor", ctx);
        if (!f.xor_with.empty() && !scalar(f.type)) fail(t, ctx + ".xor needs an integer type");
        f.charset = str(t, "charset", ctx);
        if (f.type == SymType::Text) {
            if (f.charset.empty()) f.charset = "ascii";
            if (f.charset != "ascii" && f.charset != "pokemon") fail(t, ctx + ".charset must be ascii or pokemon");
        } else if (!f.charset.empty()) {
            fail(t, ctx + ".charset is only for type text");
        }
    }

    void only(const toml::table& t, const std::string& ctx, std::initializer_list<std::string_view> keys) const {
        for (const auto& [k, node] : t) {
            if (std::find(keys.begin(), keys.end(), k.str()) == keys.end()) {
                fail(node, "unknown key '" + std::string(k.str()) + "' in " + ctx);
            }
        }
    }

private:
    std::string_view source_;
};

std::int64_t to_signed(std::uint32_t v, std::uint32_t size) {
    switch (size) {
    case 1: return static_cast<std::int8_t>(v);
    case 2: return static_cast<std::int16_t>(v);
    default: return static_cast<std::int32_t>(v);
    }
}

// Splits "party[0].hp" into ("party", 0, "hp"); index -1 when absent.
struct Path {
    std::string name;
    long index = -1;
    std::string field;
    long field_index = -1;
};

std::pair<std::string, long> name_index(std::string_view s, std::string_view whole) {
    const auto open = s.find('[');
    if (open == std::string_view::npos) return {std::string(s), -1};
    if (s.back() != ']') throw std::runtime_error("bad symbol path '" + std::string(whole) + "'");
    const std::string digits(s.substr(open + 1, s.size() - open - 2));
    char* end = nullptr;
    const long index = std::strtol(digits.c_str(), &end, 10);
    if (digits.empty() || *end != '\0' || index < 0) throw std::runtime_error("bad index in '" + std::string(whole) + "'");
    return {std::string(s.substr(0, open)), index};
}

Path parse_path(std::string_view path) {
    Path p;
    const auto dot = path.find('.');
    std::tie(p.name, p.index) = name_index(path.substr(0, dot), path);
    if (dot != std::string_view::npos) std::tie(p.field, p.field_index) = name_index(path.substr(dot + 1), path);
    return p;
}

}  // namespace

std::string decode_text(std::span<const std::uint8_t> bytes, std::string_view charset) {
    std::string out;
    for (const std::uint8_t b : bytes) {
        if (charset == "pokemon") {
            if (b == 0xFF) break;
            if (b == 0x00) out += ' ';
            else if (b >= 0xA1 && b <= 0xAA) out += static_cast<char>('0' + (b - 0xA1));
            else if (b >= 0xBB && b <= 0xD4) out += static_cast<char>('A' + (b - 0xBB));
            else if (b >= 0xD5 && b <= 0xEE) out += static_cast<char>('a' + (b - 0xD5));
            else if (b == 0xAB) out += '!';
            else if (b == 0xAC) out += '?';
            else if (b == 0xAD) out += '.';
            else if (b == 0xAE) out += '-';
            else if (b == 0xB0) out += "...";
            else if (b == 0xB1 || b == 0xB2) out += '"';
            else if (b == 0xB3 || b == 0xB4) out += '\'';
            else if (b == 0xB5) out += "(M)";
            else if (b == 0xB6) out += "(F)";
            else if (b == 0xB8) out += ',';
            else if (b == 0xBA) out += '/';
            else out += '?';
        } else {
            if (b == 0) break;
            out += b >= 0x20 && b < 0x7F ? static_cast<char>(b) : '?';
        }
    }
    return out;
}

Symbols Symbols::parse(std::string_view toml_text, std::string_view source_name) {
    toml::table root;
    try {
        root = toml::parse(toml_text, source_name);
    } catch (const toml::parse_error& e) {
        std::ostringstream msg;
        msg << source_name << ":" << e.source().begin.line << ": " << e.description();
        throw std::runtime_error(msg.str());
    }
    const Reader r(source_name);
    Symbols out;
    const toml::node* node = root.get("symbols");
    if (!node) return out;
    const auto* table = node->as_table();
    if (!table) r.fail(*node, "[symbols] must be a table of symbols");
    for (const auto& [key, value] : *table) {
        const std::string name(key.str());
        const std::string ctx = "symbols." + name;
        const auto* t = value.as_table();
        if (!t) r.fail(value, ctx + " must be a table");
        if (name.empty() || name.find_first_of("[]. ") != std::string::npos) r.fail(value, ctx + ": names cannot contain [ ] . or spaces");
        r.only(*t, ctx, {"address", "pointer", "type", "size", "count", "bit", "xor", "charset", "fields", "journal", "description"});
        Symbol s;
        s.name = name;
        r.common(*t, ctx, s, true);
        s.offset = r.u32(*t, "address", ctx);
        if (t->get("pointer")) s.pointer = r.u32(*t, "pointer", ctx);
        s.description = r.str(*t, "description", ctx);
        if (const toml::node* j = t->get("journal")) {
            if (!j->is_boolean()) r.fail(*j, ctx + ".journal must be true or false");
            s.journal = j->as_boolean()->get();
        }
        if (s.type == SymType::Struct) {
            const toml::node* fields = t->get("fields");
            const auto* ft = fields ? fields->as_table() : nullptr;
            if (!ft || ft->empty()) r.fail(*t, ctx + " is a struct and needs a fields table");
            for (const auto& [fkey, fvalue] : *ft) {
                const std::string fctx = ctx + ".fields." + std::string(fkey.str());
                const auto* f = fvalue.as_table();
                if (!f) r.fail(fvalue, fctx + " must be a table");
                r.only(*f, fctx, {"offset", "type", "size", "count", "bit", "xor", "charset"});
                SymField field;
                field.name = std::string(fkey.str());
                r.common(*f, fctx, field, false);
                field.offset = r.u32(*f, "offset", fctx);
                if (static_cast<std::uint64_t>(field.offset) + std::uint64_t{field.size} * field.count > s.size) {
                    r.fail(*f, fctx + " does not fit in the struct size " + std::to_string(s.size));
                }
                s.fields.push_back(std::move(field));
            }
            // TOML tables have no order; memory order reads naturally.
            std::stable_sort(s.fields.begin(), s.fields.end(), [](const SymField& x, const SymField& y) { return x.offset < y.offset; });
        } else if (t->get("fields")) {
            r.fail(*t, ctx + ".fields needs type = \"struct\"");
        }
        out.symbols_.push_back(std::move(s));
    }
    // xor names must exist and be scalars.
    const auto check_xor = [&](const SymField& f, const std::string& ctx) {
        if (f.xor_with.empty()) return;
        const Symbol* key = out.find(f.xor_with);
        if (!key || !scalar(key->type) || key->count != 1) {
            throw std::runtime_error(std::string(source_name) + ": " + ctx + ".xor names '" + f.xor_with +
                                     "', which is not a single integer symbol");
        }
    };
    for (const Symbol& s : out.symbols_) {
        check_xor(s, "symbols." + s.name);
        for (const SymField& f : s.fields) check_xor(f, "symbols." + s.name + ".fields." + f.name);
    }
    return out;
}

Symbols Symbols::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return parse(text, path.string());
}

void Symbols::merge(const Symbols& other) {
    for (const Symbol& s : other.symbols_) {
        const auto it = std::find_if(symbols_.begin(), symbols_.end(), [&](const Symbol& x) { return x.name == s.name; });
        if (it != symbols_.end()) *it = s;
        else symbols_.push_back(s);
    }
}

const Symbol* Symbols::find(std::string_view name) const {
    const auto it = std::find_if(symbols_.begin(), symbols_.end(), [&](const Symbol& s) { return s.name == name; });
    return it == symbols_.end() ? nullptr : &*it;
}

std::uint32_t Symbols::base(const Symbol& s, const Peek& peek) const {
    if (!s.pointer) return s.offset;
    std::uint8_t b[4];
    peek(*s.pointer, b);
    const std::uint32_t target = b[0] | (std::uint32_t{b[1]} << 8) | (std::uint32_t{b[2]} << 16) | (std::uint32_t{b[3]} << 24);
    if (target == 0) throw std::runtime_error(s.name + ": the pointer at " + hex(*s.pointer) + " is null");
    return target + s.offset;
}

std::uint32_t Symbols::xor_key(const std::string& name, const Peek& peek, int depth) const {
    const Symbol* key = find(name);
    if (!key || depth > 4) throw std::runtime_error("xor key '" + name + "' cannot be read");
    std::uint8_t b[4] = {};
    peek(base(*key, peek), std::span(b, key->size));
    std::uint32_t v = b[0] | (std::uint32_t{b[1]} << 8) | (std::uint32_t{b[2]} << 16) | (std::uint32_t{b[3]} << 24);
    if (!key->xor_with.empty()) v ^= xor_key(key->xor_with, peek, depth + 1);
    return v;
}

Json Symbols::read_value(const SymField& f, std::uint32_t addr, const Peek& peek) const {
    const auto one = [&](std::uint32_t a) -> Json {
        std::vector<std::uint8_t> bytes(f.size);
        peek(a, bytes);
        if (f.type == SymType::Bytes) return Json(hex_encode(bytes.data(), bytes.size()));
        if (f.type == SymType::Text) return Json(decode_text(bytes, f.charset));
        std::uint32_t v = 0;
        for (std::uint32_t i = 0; i < f.size; ++i) v |= std::uint32_t{bytes[i]} << (8 * i);
        if (!f.xor_with.empty()) {
            v ^= xor_key(f.xor_with, peek, 0);
            if (f.size < 4) v &= (1u << (8 * f.size)) - 1;
        }
        if (f.bit >= 0) return Json(((v >> f.bit) & 1u) != 0);
        switch (f.type) {
        case SymType::Bool: return Json(v != 0);
        case SymType::S8:
        case SymType::S16:
        case SymType::S32: return Json(to_signed(v, f.size));
        default: return Json(v);
        }
    };
    if (f.count == 1) return one(addr);
    Json arr = Json::array();
    for (std::uint32_t i = 0; i < f.count; ++i) arr.push_back(one(addr + i * f.size));
    return arr;
}

Json Symbols::read_element(const Symbol& s, std::uint32_t addr, const Peek& peek) const {
    if (s.type != SymType::Struct) {
        SymField single = s;
        single.count = 1;
        return read_value(single, addr, peek);
    }
    Json obj = Json::object();
    for (const SymField& f : s.fields) obj[f.name] = read_value(f, addr + f.offset, peek);
    return obj;
}

Json Symbols::read(std::string_view text, const Peek& peek) const {
    const Path p = parse_path(text);
    const Symbol* s = find(p.name);
    if (!s) throw std::runtime_error("no symbol '" + p.name + "' (list_symbols shows them)");
    if (p.index >= 0 && static_cast<std::uint32_t>(p.index) >= s->count) {
        throw std::runtime_error(p.name + " has " + std::to_string(s->count) + " elements");
    }
    if (p.index < 0 && s->count > 1 && !p.field.empty()) {
        throw std::runtime_error(p.name + " is an array; use " + p.name + "[i]." + p.field);
    }
    const std::uint32_t start = base(*s, peek);
    if (p.field.empty()) {
        if (p.index >= 0) return read_element(*s, start + static_cast<std::uint32_t>(p.index) * s->size, peek);
        if (s->count == 1) return read_element(*s, start, peek);
        Json arr = Json::array();
        for (std::uint32_t i = 0; i < s->count; ++i) arr.push_back(read_element(*s, start + i * s->size, peek));
        return arr;
    }
    const auto it = std::find_if(s->fields.begin(), s->fields.end(), [&](const SymField& f) { return f.name == p.field; });
    if (it == s->fields.end()) throw std::runtime_error(p.name + " has no field '" + p.field + "'");
    const std::uint32_t elem = start + static_cast<std::uint32_t>(std::max(p.index, 0L)) * s->size + it->offset;
    if (p.field_index >= 0) {
        if (static_cast<std::uint32_t>(p.field_index) >= it->count) {
            throw std::runtime_error(p.field + " has " + std::to_string(it->count) + " elements");
        }
        SymField single = *it;
        single.count = 1;
        return read_value(single, elem + static_cast<std::uint32_t>(p.field_index) * it->size, peek);
    }
    return read_value(*it, elem, peek);
}

Json Symbols::describe(const Peek& peek) const {
    Json out = Json::array();
    for (const Symbol& s : symbols_) {
        Json d = Json::object();
        d["name"] = s.name;
        d["type"] = std::string(type_name(s.type));
        if (s.count > 1) d["count"] = s.count;
        if (!scalar(s.type)) d["size"] = s.size;
        if (s.pointer) {
            d["pointer"] = hex(*s.pointer);
            d["offset"] = hex(s.offset);
        }
        try {
            d["address"] = hex(base(s, peek));
        } catch (const std::exception& e) {
            d["address"] = std::string("unresolved: ") + e.what();
        }
        if (!s.description.empty()) d["description"] = s.description;
        if (!s.fields.empty()) {
            Json fields = Json::array();
            for (const SymField& f : s.fields) {
                std::string text = f.name + ": " + std::string(type_name(f.type));
                if (f.count > 1) text += "[" + std::to_string(f.count) + "]";
                fields.push_back(text);
            }
            d["fields"] = std::move(fields);
        }
        out.push_back(std::move(d));
    }
    return out;
}

Json Symbols::snapshot(const Peek& peek) const {
    Json out = Json::object();
    for (const Symbol& s : symbols_) {
        if (!s.journal) continue;
        try {
            out[s.name] = read(s.name, peek);
        } catch (const std::exception& e) {
            out[s.name] = Json(Json::Object{{"error", e.what()}});
        }
    }
    return out;
}

}  // namespace anygba::control
