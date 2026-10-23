#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace anygba {

// Lower-case hex SHA-256 of `data`. Used to tie generated code to the ROM it came from.
std::string sha256_hex(std::span<const std::uint8_t> data);

}  // namespace anygba
