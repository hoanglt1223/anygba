#include "bios/bios.h"

#include <cstdio>
#include <vector>

// Decompressors (GBATEK "BIOS Decompression Functions"). "Wram" variants store bytes,
// "Vram" variants pair bytes into halfword stores, since VRAM ignores or doubles byte
// writes. Sources inside the BIOS region are rejected silently, as the BIOS does.
namespace anygba::bios {

namespace {

class Writer {
public:
    Writer(System& sys, std::uint32_t dst, bool halfwords) : sys_(sys), dst_(dst), halfwords_(halfwords) {}

    void put(std::uint8_t b) {
        if (!halfwords_) {
            sys_.write8(dst_++, b);
            return;
        }
        if (pending_) {
            sys_.write16(dst_ & ~1u, static_cast<std::uint32_t>(low_ | (b << 8)));
            dst_ += 2;
            pending_ = false;
        } else {
            low_ = b;
            pending_ = true;
        }
    }

private:
    System& sys_;
    std::uint32_t dst_;
    bool halfwords_;
    bool pending_ = false;
    std::uint8_t low_ = 0;
};

bool bad_source(std::uint32_t src) { return (src & 0x0E000000) == 0; }

std::uint8_t byte_at(System& sys, std::uint32_t addr) { return static_cast<std::uint8_t>(sys.read8(addr)); }

}  // namespace

void lz77(System& sys, bool vram) {
    anygba_cpu& c = sys.cpu();
    std::uint32_t src = c.r[0];
    if (bad_source(src)) {
        return;
    }
    const std::uint32_t size = sys.read32(src) >> 8;
    src += 4;
    std::vector<std::uint8_t> out;
    out.reserve(size);
    Writer w(sys, c.r[1], vram);
    while (out.size() < size) {
        const std::uint8_t flags = byte_at(sys, src++);
        for (int bit = 7; bit >= 0 && out.size() < size; --bit) {
            if (!((flags >> bit) & 1)) {
                out.push_back(byte_at(sys, src++));
                w.put(out.back());
                continue;
            }
            const std::uint8_t hi = byte_at(sys, src++);
            const std::uint8_t lo = byte_at(sys, src++);
            const std::size_t length = static_cast<std::size_t>(hi >> 4) + 3;
            const std::size_t disp = (static_cast<std::size_t>(hi & 0xF) << 8 | lo) + 1;
            for (std::size_t i = 0; i < length && out.size() < size; ++i) {
                // A reference before the start of the output reads what is already there.
                const std::uint8_t b = disp <= out.size()
                                           ? out[out.size() - disp]
                                           : byte_at(sys, c.r[1] + static_cast<std::uint32_t>(out.size() - disp));
                out.push_back(b);
                w.put(b);
            }
        }
    }
    spend(sys, 40 + static_cast<std::int64_t>(size) * (vram ? 12 : 10));
}

void huffman(System& sys) {
    anygba_cpu& c = sys.cpu();
    const std::uint32_t src = c.r[0];
    if (bad_source(src)) {
        return;
    }
    const std::uint32_t header = sys.read32(src);
    const std::uint32_t size = header >> 8;
    const std::uint32_t bits = header & 0xF;
    if (bits == 0 || bits > 8 || (8 % bits) != 0) {
        char msg[96];
        std::snprintf(msg, sizeof msg, "BIOS HuffUnComp: unsupported data size %u bits at 0x%08X", bits, src);
        sys.fail(msg);
    }
    const std::uint32_t tree = src + 5;
    std::uint32_t stream = src + 4 + (static_cast<std::uint32_t>(byte_at(sys, src + 4)) + 1) * 2;
    std::uint32_t dst = c.r[1];

    std::uint32_t node = tree;
    std::uint32_t word = 0;
    std::uint32_t word_bits = 0;
    std::uint32_t written = 0;
    while (written < size) {
        const std::uint32_t chunk = sys.read32(stream);
        stream += 4;
        for (int i = 31; i >= 0 && written < size; --i) {
            const std::uint32_t dir = (chunk >> i) & 1;
            const std::uint8_t value = byte_at(sys, node);
            const std::uint32_t child = (node & ~1u) + (value & 0x3Fu) * 2 + 2 + dir;
            const bool leaf = ((value >> (7 - dir)) & 1) != 0;
            if (!leaf) {
                node = child;
                continue;
            }
            word |= (static_cast<std::uint32_t>(byte_at(sys, child)) & ((1u << bits) - 1)) << word_bits;
            word_bits += bits;
            node = tree;
            if (word_bits == 32) {
                sys.write32(dst, word);
                dst += 4;
                written += 4;
                word = 0;
                word_bits = 0;
            }
        }
    }
    spend(sys, 40 + static_cast<std::int64_t>(size) * 24);
}

void run_length(System& sys, bool vram) {
    anygba_cpu& c = sys.cpu();
    std::uint32_t src = c.r[0];
    if (bad_source(src)) {
        return;
    }
    const std::uint32_t size = sys.read32(src) >> 8;
    src += 4;
    Writer w(sys, c.r[1], vram);
    std::uint32_t written = 0;
    while (written < size) {
        const std::uint8_t flag = byte_at(sys, src++);
        if (flag & 0x80) {
            const std::uint32_t n = (flag & 0x7Fu) + 3;
            const std::uint8_t b = byte_at(sys, src++);
            for (std::uint32_t i = 0; i < n && written < size; ++i, ++written) {
                w.put(b);
            }
        } else {
            const std::uint32_t n = (flag & 0x7Fu) + 1;
            for (std::uint32_t i = 0; i < n && written < size; ++i, ++written) {
                w.put(byte_at(sys, src++));
            }
        }
    }
    spend(sys, 40 + static_cast<std::int64_t>(size) * (vram ? 10 : 8));
}

void diff_unfilter(System& sys, int unit_bytes, bool vram) {
    anygba_cpu& c = sys.cpu();
    std::uint32_t src = c.r[0];
    if (bad_source(src)) {
        return;
    }
    const std::uint32_t size = sys.read32(src) >> 8;
    src += 4;
    std::uint32_t dst = c.r[1];
    if (unit_bytes == 2) {
        std::uint32_t acc = 0;
        for (std::uint32_t i = 0; i < size; i += 2) {
            acc = (acc + sys.read16(src + i)) & 0xFFFF;
            sys.write16(dst + i, acc);
        }
    } else {
        Writer w(sys, dst, vram);
        std::uint8_t acc = 0;
        for (std::uint32_t i = 0; i < size; ++i) {
            acc = static_cast<std::uint8_t>(acc + byte_at(sys, src + i));
            w.put(acc);
        }
    }
    spend(sys, 40 + static_cast<std::int64_t>(size) * 6);
}

void bit_unpack(System& sys) {
    anygba_cpu& c = sys.cpu();
    std::uint32_t src = c.r[0];
    if (bad_source(src)) {
        return;
    }
    const std::uint32_t info = c.r[2];
    const std::uint32_t length = sys.read16(info);
    const std::uint32_t src_width = sys.read8(info + 2);
    const std::uint32_t dst_width = sys.read8(info + 3);
    const std::uint32_t offset_word = sys.read32(info + 4);
    const std::uint32_t offset = offset_word & 0x7FFFFFFF;
    const bool offset_zero = (offset_word >> 31) != 0;
    const bool src_ok = src_width == 1 || src_width == 2 || src_width == 4 || src_width == 8;
    const bool dst_ok = dst_width == 1 || dst_width == 2 || dst_width == 4 || dst_width == 8 || dst_width == 16 ||
                        dst_width == 32;
    if (!src_ok || !dst_ok) {
        char msg[96];
        std::snprintf(msg, sizeof msg, "BIOS BitUnPack: unsupported widths %u -> %u", src_width, dst_width);
        sys.fail(msg);
    }
    std::uint32_t dst = c.r[1] & ~3u;
    std::uint64_t word = 0;
    std::uint32_t word_bits = 0;
    for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint8_t b = byte_at(sys, src + i);
        for (std::uint32_t bit = 0; bit < 8; bit += src_width) {
            std::uint32_t unit = (b >> bit) & ((1u << src_width) - 1);
            if (unit != 0 || offset_zero) {
                unit += offset;
            }
            word |= static_cast<std::uint64_t>(unit) << word_bits;
            word_bits += dst_width;
            if (word_bits >= 32) {
                sys.write32(dst, static_cast<std::uint32_t>(word));
                dst += 4;
                word = 0;
                word_bits = 0;
            }
        }
    }
    spend(sys, 40 + static_cast<std::int64_t>(length) * 8 * 4);
}

}  // namespace anygba::bios
