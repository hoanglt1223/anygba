#include "support/elf_symbols.h"

#include <stdexcept>

namespace anygba::test {

namespace {

struct Reader {
    std::span<const std::uint8_t> b;

    void need(std::uint64_t off, std::uint64_t n) const {
        if (off + n > b.size()) {
            throw std::runtime_error("ELF: truncated file");
        }
    }
    std::uint16_t u16(std::uint64_t off) const {
        need(off, 2);
        return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
    }
    std::uint32_t u32(std::uint64_t off) const {
        need(off, 4);
        return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
               (static_cast<std::uint32_t>(b[off + 2]) << 16) | (static_cast<std::uint32_t>(b[off + 3]) << 24);
    }
    std::string str(std::uint64_t off) const {
        std::string s;
        for (;; ++off) {
            need(off, 1);
            if (b[off] == 0) {
                return s;
            }
            s += static_cast<char>(b[off]);
        }
    }
};

}  // namespace

ElfFile parse_elf(std::span<const std::uint8_t> bytes) {
    const Reader r{bytes};
    r.need(0, 52);
    if (bytes[0] != 0x7F || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F' || bytes[4] != 1 || bytes[5] != 1) {
        throw std::runtime_error("ELF: not a little-endian ELF32 file");
    }
    const std::uint32_t shoff = r.u32(32);
    const std::uint16_t shentsize = r.u16(46);
    const std::uint16_t shnum = r.u16(48);
    const std::uint16_t shstrndx = r.u16(50);
    if (shentsize < 40 || shstrndx >= shnum) {
        throw std::runtime_error("ELF: bad section header table");
    }
    const auto sh = [&](unsigned i) { return static_cast<std::uint64_t>(shoff) + static_cast<std::uint64_t>(i) * shentsize; };
    const std::uint32_t shstr_off = r.u32(sh(shstrndx) + 16);

    ElfFile elf;
    std::uint32_t symtab = 0;
    for (unsigned i = 0; i < shnum; ++i) {
        ElfSection s;
        s.name = r.str(static_cast<std::uint64_t>(shstr_off) + r.u32(sh(i)));
        s.type = r.u32(sh(i) + 4);
        s.flags = r.u32(sh(i) + 8);
        s.addr = r.u32(sh(i) + 12);
        s.size = r.u32(sh(i) + 20);
        if (s.type == 2) {
            symtab = i;
        }
        elf.sections.push_back(std::move(s));
    }
    if (symtab == 0) {
        return elf;
    }
    const std::uint32_t sym_off = r.u32(sh(symtab) + 16);
    const std::uint32_t sym_size = r.u32(sh(symtab) + 20);
    const std::uint32_t str_link = r.u32(sh(symtab) + 24);
    if (str_link >= shnum) {
        throw std::runtime_error("ELF: bad symbol string table link");
    }
    const std::uint32_t str_off = r.u32(sh(str_link) + 16);
    for (std::uint64_t off = sym_off; off + 16 <= static_cast<std::uint64_t>(sym_off) + sym_size; off += 16) {
        ElfSymbol sym;
        sym.name = r.str(static_cast<std::uint64_t>(str_off) + r.u32(off));
        sym.value = r.u32(off + 4);
        sym.size = r.u32(off + 8);
        r.need(off + 12, 4);
        sym.type = static_cast<std::uint8_t>(bytes[off + 12] & 0xF);
        sym.section = r.u16(off + 14);
        elf.symbols.push_back(std::move(sym));
    }
    return elf;
}

}  // namespace anygba::test
