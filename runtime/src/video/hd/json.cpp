#include "video/hd/json.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdexcept>

namespace anygba::video::hd::json {

namespace {

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    Value document() {
        Value v = value();
        space();
        if (i_ != t_.size()) fail("text after the end");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error("JSON: " + what + " at offset " + std::to_string(i_));
    }
    void space() {
        while (i_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[i_]))) ++i_;
    }
    bool eat(char c) {
        space();
        if (i_ < t_.size() && t_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    void expect(char c) {
        if (!eat(c)) fail(std::string("expected '") + c + "'");
    }
    bool word(std::string_view w) {
        if (t_.substr(i_, w.size()) == w) {
            i_ += w.size();
            return true;
        }
        return false;
    }

    Value value() {
        if (++depth_ > 64) fail("nesting too deep");
        space();
        if (i_ >= t_.size()) fail("unexpected end");
        Value v;
        const char c = t_[i_];
        if (c == '{') {
            ++i_;
            v.type = Value::Type::Object;
            if (!eat('}')) {
                do {
                    space();
                    std::string key = string();
                    expect(':');
                    v.object[std::move(key)] = value();
                } while (eat(','));
                expect('}');
            }
        } else if (c == '[') {
            ++i_;
            v.type = Value::Type::Array;
            if (!eat(']')) {
                do {
                    v.array.push_back(value());
                } while (eat(','));
                expect(']');
            }
        } else if (c == '"') {
            v.type = Value::Type::String;
            v.string = string();
        } else if (word("true")) {
            v.type = Value::Type::Bool;
            v.boolean = true;
        } else if (word("false")) {
            v.type = Value::Type::Bool;
        } else if (word("null")) {
        } else {
            v.type = Value::Type::Number;
            // strtod on a copy, not from_chars: the NDK's libc++ has no floating-point from_chars.
            std::size_t n = 0;
            while (i_ + n < t_.size() && std::strchr("0123456789+-.eE", t_[i_ + n]) != nullptr) ++n;
            const std::string text(t_.substr(i_, n));
            char* end = nullptr;
            v.number = text.empty() ? 0 : std::strtod(text.c_str(), &end);
            if (text.empty() || end != text.c_str() + text.size()) fail("bad value");
            i_ += n;
        }
        --depth_;
        return v;
    }

    std::string string() {
        if (i_ >= t_.size() || t_[i_] != '"') fail("expected a string");
        ++i_;
        std::string out;
        while (true) {
            if (i_ >= t_.size()) fail("unterminated string");
            const char c = t_[i_++];
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= t_.size()) fail("unterminated escape");
            const char e = t_[i_++];
            switch (e) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i_ + 4 > t_.size()) fail("short \\u escape");
                unsigned cp = 0;
                const auto [end, ec] = std::from_chars(t_.data() + i_, t_.data() + i_ + 4, cp, 16);
                if (ec != std::errc{} || end != t_.data() + i_ + 4) fail("bad \\u escape");
                i_ += 4;
                // Basic plane only (the index never writes surrogates).
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: fail("bad escape");
            }
        }
        return out;
    }

    std::string_view t_;
    std::size_t i_ = 0;
    int depth_ = 0;
};

}  // namespace

Value parse(std::string_view text) { return Parser(text).document(); }

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

}  // namespace anygba::video::hd::json
