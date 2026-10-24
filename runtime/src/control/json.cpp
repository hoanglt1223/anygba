#include "control/json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace anygba::control {

namespace {

const char* type_name(Json::Type t) {
    switch (t) {
    case Json::Type::Null: return "null";
    case Json::Type::Bool: return "a boolean";
    case Json::Type::Int:
    case Json::Type::Double: return "a number";
    case Json::Type::String: return "a string";
    case Json::Type::Array: return "an array";
    case Json::Type::Object: return "an object";
    }
    return "?";
}

[[noreturn]] void mismatch(Json::Type want, Json::Type got) {
    throw JsonError(std::string("expected ") + type_name(want) + ", got " + type_name(got));
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Json document() {
        Json v = value(0);
        skip_ws();
        if (pos_ != s_.size()) fail("unexpected text after the value");
        return v;
    }

private:
    static constexpr int kMaxDepth = 64;

    [[noreturn]] void fail(const std::string& what) const {
        throw JsonError("JSON: " + what + " at offset " + std::to_string(pos_));
    }

    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }

    bool eat(char c) {
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char c) {
        if (!eat(c)) fail(std::string("expected '") + c + "'");
    }

    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) == word) {
            pos_ += word.size();
            return true;
        }
        return false;
    }

    Json value(int depth) {
        if (depth > kMaxDepth) fail("nesting too deep");
        skip_ws();
        if (pos_ >= s_.size()) fail("unexpected end");
        const char c = s_[pos_];
        if (c == '{') return object(depth);
        if (c == '[') return array(depth);
        if (c == '"') return Json(string());
        if (literal("true")) return Json(true);
        if (literal("false")) return Json(false);
        if (literal("null")) return Json();
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        fail("unexpected character");
    }

    Json object(int depth) {
        ++pos_;
        Json::Object members;
        if (eat('}')) return Json(std::move(members));
        do {
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') fail("expected a member name");
            std::string key = string();
            expect(':');
            members.emplace_back(std::move(key), value(depth + 1));
        } while (eat(','));
        expect('}');
        return Json(std::move(members));
    }

    Json array(int depth) {
        ++pos_;
        Json::Array items;
        if (eat(']')) return Json(std::move(items));
        do {
            items.push_back(value(depth + 1));
        } while (eat(','));
        expect(']');
        return Json(std::move(items));
    }

    std::uint32_t hex4() {
        if (pos_ + 4 > s_.size()) fail("short \\u escape");
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<std::uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<std::uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<std::uint32_t>(h - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }

    std::string string() {
        ++pos_;  // opening quote
        std::string out;
        for (;;) {
            if (pos_ >= s_.size()) fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00) {
                    if (!literal("\\u")) fail("lone surrogate");
                    const std::uint32_t lo = hex4();
                    if (lo < 0xDC00 || lo >= 0xE000) fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    fail("lone surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default: fail("bad escape");
            }
        }
    }

    Json number() {
        const std::size_t start = pos_;
        bool integral = true;
        if (s_[pos_] == '-') ++pos_;
        const auto digits = [&] {
            const std::size_t from = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (pos_ == from) fail("bad number");
        };
        digits();
        if (pos_ < s_.size() && s_[pos_] == '.') {
            integral = false;
            ++pos_;
            digits();
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            integral = false;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            digits();
        }
        const std::string text(s_.substr(start, pos_ - start));
        if (integral) {
            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(text.c_str(), &end, 10);
            if (errno == 0 && *end == '\0') return Json(static_cast<std::int64_t>(v));
        }
        return Json(std::strtod(text.c_str(), nullptr));
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

void dump_string(std::string& out, const std::string& s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}

}  // namespace

bool Json::as_bool() const {
    if (!is_bool()) mismatch(Type::Bool, type());
    return std::get<bool>(v_);
}

std::int64_t Json::as_int() const {
    if (is_int()) return std::get<std::int64_t>(v_);
    if (type() == Type::Double) {
        const double d = std::get<double>(v_);
        if (std::floor(d) == d && std::fabs(d) < 9.2e18) return static_cast<std::int64_t>(d);
        throw JsonError("expected an integer");
    }
    mismatch(Type::Int, type());
}

double Json::as_double() const {
    if (is_int()) return static_cast<double>(std::get<std::int64_t>(v_));
    if (type() == Type::Double) return std::get<double>(v_);
    mismatch(Type::Double, type());
}

const std::string& Json::as_string() const {
    if (!is_string()) mismatch(Type::String, type());
    return std::get<std::string>(v_);
}

const Json::Array& Json::as_array() const {
    if (!is_array()) mismatch(Type::Array, type());
    return std::get<Array>(v_);
}

Json::Array& Json::as_array() {
    if (!is_array()) mismatch(Type::Array, type());
    return std::get<Array>(v_);
}

const Json::Object& Json::as_object() const {
    if (!is_object()) mismatch(Type::Object, type());
    return std::get<Object>(v_);
}

Json::Object& Json::as_object() {
    if (!is_object()) mismatch(Type::Object, type());
    return std::get<Object>(v_);
}

const Json* Json::find(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (const auto& [k, v] : std::get<Object>(v_)) {
        if (k == key) return &v;
    }
    return nullptr;
}

Json& Json::operator[](std::string_view key) {
    if (is_null()) v_ = Object{};
    Object& o = as_object();
    for (auto& [k, v] : o) {
        if (k == key) return v;
    }
    o.emplace_back(std::string(key), Json());
    return o.back().second;
}

void Json::push_back(Json value) {
    if (is_null()) v_ = Array{};
    as_array().push_back(std::move(value));
}

std::size_t Json::size() const {
    if (is_array()) return std::get<Array>(v_).size();
    if (is_object()) return std::get<Object>(v_).size();
    return 0;
}

std::string Json::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

void Json::dump_to(std::string& out) const {
    switch (type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += std::get<bool>(v_) ? "true" : "false"; break;
    case Type::Int: out += std::to_string(std::get<std::int64_t>(v_)); break;
    case Type::Double: {
        const double d = std::get<double>(v_);
        if (!std::isfinite(d)) {
            out += "null";
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.17g", d);
            out += buf;
        }
        break;
    }
    case Type::String: dump_string(out, std::get<std::string>(v_)); break;
    case Type::Array: {
        out += '[';
        bool first = true;
        for (const Json& item : std::get<Array>(v_)) {
            if (!first) out += ',';
            first = false;
            item.dump_to(out);
        }
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& [k, v] : std::get<Object>(v_)) {
            if (!first) out += ',';
            first = false;
            dump_string(out, k);
            out += ':';
            v.dump_to(out);
        }
        out += '}';
        break;
    }
    }
}

Json Json::parse(std::string_view text) { return Parser(text).document(); }

std::string base64_encode(const std::uint8_t* data, std::size_t size) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const std::uint32_t v = (std::uint32_t{data[i]} << 16) | (std::uint32_t{data[i + 1]} << 8) | data[i + 2];
        out += kTable[v >> 18];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += kTable[v & 63];
    }
    if (i < size) {
        std::uint32_t v = std::uint32_t{data[i]} << 16;
        if (i + 1 < size) v |= std::uint32_t{data[i + 1]} << 8;
        out += kTable[v >> 18];
        out += kTable[(v >> 12) & 63];
        out += i + 1 < size ? kTable[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string hex_encode(const std::uint8_t* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += kDigits[data[i] >> 4];
        out += kDigits[data[i] & 15];
    }
    return out;
}

std::vector<std::uint8_t> hex_decode(std::string_view hex) {
    std::vector<std::uint8_t> out;
    int high = -1;
    for (const char c : hex) {
        if (c == ' ') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else throw JsonError(std::string("not a hex digit: '") + c + "'");
        if (high < 0) {
            high = v;
        } else {
            out.push_back(static_cast<std::uint8_t>(high << 4 | v));
            high = -1;
        }
    }
    if (high >= 0) throw JsonError("odd number of hex digits");
    return out;
}

}  // namespace anygba::control
