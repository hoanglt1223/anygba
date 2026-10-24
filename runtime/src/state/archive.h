#pragma once

#include "anygba/system.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

// One serialization routine per component serves both directions: an Archive either appends
// the bytes it is shown (saving) or overwrites them from a state image (loading). Values are
// stored in host byte order; every supported host is little-endian.
namespace anygba::state {

class Archive {
public:
    static Archive writer() { return Archive(); }
    static Archive reader(std::span<const std::uint8_t> image) { return Archive(image); }

    bool loading() const { return loading_; }

    void raw(void* data, std::size_t size) {
        if (!loading_) {
            const auto* p = static_cast<const std::uint8_t*>(data);
            out_.insert(out_.end(), p, p + size);
            return;
        }
        if (size > in_.size() - pos_) {
            throw Error("state file is truncated");
        }
        std::memcpy(data, in_.data() + pos_, size);
        pos_ += size;
    }

    template <class T>
    void pod(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        raw(&value, sizeof value);
    }

    // Stored as one byte, 0 or 1.
    void flag(bool& value) {
        std::uint8_t b = value ? 1 : 0;
        pod(b);
        if (loading_) {
            if (b > 1) {
                throw Error("state file has an invalid flag");
            }
            value = b != 0;
        }
    }

    template <class... T>
    void fields(T&... values) {
        (field(values), ...);
    }

    // A buffer whose size the current object already fixes (RAM, chip contents).
    void fixed(std::vector<std::uint8_t>& bytes, const char* what) {
        auto size = static_cast<std::uint32_t>(bytes.size());
        pod(size);
        if (loading_ && size != bytes.size()) {
            throw Error(std::string("state file ") + what + " is " + std::to_string(size) + " bytes, expected " +
                        std::to_string(bytes.size()));
        }
        raw(bytes.data(), bytes.size());
    }

    // A buffer whose size is part of the state, at most `max` bytes.
    void sized(std::vector<std::uint8_t>& bytes, std::size_t max, const char* what) {
        auto size = static_cast<std::uint32_t>(bytes.size());
        pod(size);
        if (loading_) {
            if (size > max) {
                throw Error(std::string("state file ") + what + " is too large (" + std::to_string(size) + " bytes)");
            }
            bytes.resize(size);
        }
        raw(bytes.data(), bytes.size());
    }

    bool at_end() const { return pos_ == in_.size(); }
    std::vector<std::uint8_t> take() { return std::move(out_); }

private:
    void field(bool& value) { flag(value); }
    template <class T>
    void field(T& value) {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T> || std::is_array_v<T>,
                      "structs with padding must list their fields");
        pod(value);
    }

    Archive() = default;
    explicit Archive(std::span<const std::uint8_t> image) : loading_(true), in_(image) {}

    bool loading_ = false;
    std::vector<std::uint8_t> out_;
    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
};

}  // namespace anygba::state
