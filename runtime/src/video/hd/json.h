#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Just enough JSON for the HD dump index: parse and write objects, arrays, strings, numbers,
// booleans and null.
namespace anygba::video::hd::json {

struct Value {
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string, Value> object;

    const Value* get(const std::string& key) const {
        const auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }
    std::string str(const std::string& key, std::string fallback = {}) const {
        const Value* v = get(key);
        return v && v->type == Type::String ? v->string : fallback;
    }
    double num(const std::string& key, double fallback = 0) const {
        const Value* v = get(key);
        return v && v->type == Type::Number ? v->number : fallback;
    }
};

// Throws std::runtime_error with the offset of the problem.
Value parse(std::string_view text);

// A JSON string literal, quotes included.
std::string quote(std::string_view s);

}  // namespace anygba::video::hd::json
