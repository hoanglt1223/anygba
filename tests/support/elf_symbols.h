#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace anygba::test {

// Minimal little-endian ELF32 reader for homebrew ground truth: section headers and
// the static symbol table.
struct ElfSection {
    std::string name;
    std::uint32_t addr = 0;
    std::uint32_t size = 0;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
};

struct ElfSymbol {
    std::string name;
    std::uint32_t value = 0;
    std::uint32_t size = 0;
    std::uint8_t type = 0;  // STT_*
    std::uint16_t section = 0;
};

inline constexpr std::uint8_t kSttFunc = 2;
inline constexpr std::uint32_t kShfExecinstr = 4;

struct ElfFile {
    std::vector<ElfSection> sections;
    std::vector<ElfSymbol> symbols;
};

// Throws std::runtime_error on anything that is not a well-formed ELF32 LE file.
ElfFile parse_elf(std::span<const std::uint8_t> bytes);

}  // namespace anygba::test
