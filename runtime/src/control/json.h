#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// A small JSON value for the control protocol, the journal and anygba-mcp: parse, build,
// serialize. Objects keep insertion order. Integers without a fraction or exponent stay exact.
namespace anygba::control {

class JsonError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;
    enum class Type : std::uint8_t { Null, Bool, Int, Double, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : v_(b) {}
    Json(int i) : v_(static_cast<std::int64_t>(i)) {}
    Json(unsigned i) : v_(static_cast<std::int64_t>(i)) {}
    Json(long i) : v_(static_cast<std::int64_t>(i)) {}
    Json(unsigned long i) : v_(static_cast<std::int64_t>(i)) {}
    Json(long long i) : v_(static_cast<std::int64_t>(i)) {}
    Json(unsigned long long i) : v_(static_cast<std::int64_t>(i)) {}
    Json(double d) : v_(d) {}
    Json(const char* s) : v_(std::string(s)) {}
    Json(std::string_view s) : v_(std::string(s)) {}
    Json(std::string s) : v_(std::move(s)) {}
    Json(Array a) : v_(std::move(a)) {}
    Json(Object o) : v_(std::move(o)) {}

    static Json object() { return Json(Object{}); }
    static Json array() { return Json(Array{}); }

    Type type() const { return static_cast<Type>(v_.index()); }
    bool is_null() const { return type() == Type::Null; }
    bool is_bool() const { return type() == Type::Bool; }
    bool is_int() const { return type() == Type::Int; }
    bool is_number() const { return type() == Type::Int || type() == Type::Double; }
    bool is_string() const { return type() == Type::String; }
    bool is_array() const { return type() == Type::Array; }
    bool is_object() const { return type() == Type::Object; }

    // Throw JsonError on a type mismatch.
    bool as_bool() const;
    std::int64_t as_int() const;  // a Double with an integral value is accepted
    double as_double() const;
    const std::string& as_string() const;
    const Array& as_array() const;
    Array& as_array();
    const Object& as_object() const;
    Object& as_object();

    // Object lookup; null when absent or when this is not an object.
    const Json* find(std::string_view key) const;
    // Object member, inserted as null when absent. Turns a null value into an object.
    Json& operator[](std::string_view key);
    // Array append. Turns a null value into an array.
    void push_back(Json value);
    std::size_t size() const;

    std::string dump() const;
    static Json parse(std::string_view text);

    friend bool operator==(const Json& a, const Json& b) { return a.v_ == b.v_; }

private:
    void dump_to(std::string& out) const;
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> v_{nullptr};
};

std::string base64_encode(const std::uint8_t* data, std::size_t size);
std::string hex_encode(const std::uint8_t* data, std::size_t size);
// Throws JsonError for odd length or non-hex characters (spaces are skipped).
std::vector<std::uint8_t> hex_decode(std::string_view hex);

}  // namespace anygba::control
