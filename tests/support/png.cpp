#include "support/png.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace anygba::test {

namespace {

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s));
}

std::uint32_t get32(const std::vector<std::uint8_t>& d, std::size_t pos) {
    return (std::uint32_t{d.at(pos)} << 24) | (std::uint32_t{d.at(pos + 1)} << 16) | (std::uint32_t{d.at(pos + 2)} << 8) |
           d.at(pos + 3);
}

void chunk(std::vector<std::uint8_t>& out, const char* tag, const std::vector<std::uint8_t>& data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(tag, tag + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()));
}

// Minimal inflate (RFC 1951): stored, fixed and dynamic Huffman blocks.
class Inflater {
public:
    explicit Inflater(const std::vector<std::uint8_t>& in) : in_(in) {}

    std::vector<std::uint8_t> run() {
        bits_need(16);  // zlib header
        bool last = false;
        while (!last) {
            last = bits(1) != 0;
            switch (bits(2)) {
            case 0: stored(); break;
            case 1: fixed(); break;
            case 2: dynamic(); break;
            default: throw std::runtime_error("PNG: bad deflate block type");
            }
        }
        return out_;
    }

private:
    struct Huffman {
        std::array<std::uint16_t, 16> count{};
        std::vector<std::uint16_t> symbol;
    };

    void bits_need(int n) {
        for (int i = 0; i < n; ++i) bits(1);
    }

    unsigned bits(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; ++i) {
            if (bitpos_ == 0) {
                if (pos_ >= in_.size()) throw std::runtime_error("PNG: truncated deflate data");
                cur_ = in_[pos_++];
            }
            v |= ((cur_ >> bitpos_) & 1u) << i;
            bitpos_ = (bitpos_ + 1) & 7;
        }
        return v;
    }

    static Huffman build(const std::uint8_t* lengths, std::size_t n) {
        Huffman h;
        for (std::size_t i = 0; i < n; ++i) h.count[lengths[i]]++;
        h.count[0] = 0;
        std::array<std::uint16_t, 16> offs{};
        for (int len = 1; len < 16; ++len) offs[len] = static_cast<std::uint16_t>(offs[len - 1] + h.count[len - 1]);
        h.symbol.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            if (lengths[i]) h.symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
        }
        return h;
    }

    unsigned decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(bits(1));
            const int count = h.count[len];
            if (code - count < first) return h.symbol[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        throw std::runtime_error("PNG: bad Huffman code");
    }

    void stored() {
        bitpos_ = 0;
        if (pos_ + 4 > in_.size()) throw std::runtime_error("PNG: truncated stored block");
        const unsigned len = in_[pos_] | (in_[pos_ + 1] << 8);
        pos_ += 4;
        if (pos_ + len > in_.size()) throw std::runtime_error("PNG: truncated stored block");
        out_.insert(out_.end(), in_.begin() + static_cast<std::ptrdiff_t>(pos_),
                    in_.begin() + static_cast<std::ptrdiff_t>(pos_ + len));
        pos_ += len;
    }

    void codes(const Huffman& lit, const Huffman& dist) {
        static constexpr std::uint16_t kLenBase[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::uint8_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                     2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr std::uint16_t kDistBase[] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                      33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            const unsigned sym = decode(lit);
            if (sym < 256) {
                out_.push_back(static_cast<std::uint8_t>(sym));
            } else if (sym == 256) {
                return;
            } else {
                const unsigned li = sym - 257;
                if (li >= 29) throw std::runtime_error("PNG: bad length code");
                const unsigned len = kLenBase[li] + bits(kLenExtra[li]);
                const unsigned di = decode(dist);
                if (di >= 30) throw std::runtime_error("PNG: bad distance code");
                const unsigned d = kDistBase[di] + bits(kDistExtra[di]);
                if (d > out_.size()) throw std::runtime_error("PNG: distance past the start");
                for (unsigned i = 0; i < len; ++i) out_.push_back(out_[out_.size() - d]);
            }
        }
    }

    void fixed() {
        std::uint8_t lengths[288 + 30];
        std::fill(lengths, lengths + 144, std::uint8_t{8});
        std::fill(lengths + 144, lengths + 256, std::uint8_t{9});
        std::fill(lengths + 256, lengths + 280, std::uint8_t{7});
        std::fill(lengths + 280, lengths + 288, std::uint8_t{8});
        std::fill(lengths + 288, lengths + 318, std::uint8_t{5});
        codes(build(lengths, 288), build(lengths + 288, 30));
    }

    void dynamic() {
        static constexpr std::uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const unsigned nlen = bits(5) + 257;
        const unsigned ndist = bits(5) + 1;
        const unsigned ncode = bits(4) + 4;
        std::uint8_t lengths[320] = {};
        for (unsigned i = 0; i < ncode; ++i) lengths[kOrder[i]] = static_cast<std::uint8_t>(bits(3));
        const Huffman lencode = build(lengths, 19);
        std::fill(std::begin(lengths), std::end(lengths), std::uint8_t{0});
        for (unsigned i = 0; i < nlen + ndist;) {
            const unsigned sym = decode(lencode);
            if (sym < 16) {
                lengths[i++] = static_cast<std::uint8_t>(sym);
                continue;
            }
            std::uint8_t value = 0;
            unsigned repeat;
            if (sym == 16) {
                if (i == 0) throw std::runtime_error("PNG: repeat with no previous length");
                value = lengths[i - 1];
                repeat = 3 + bits(2);
            } else if (sym == 17) {
                repeat = 3 + bits(3);
            } else {
                repeat = 11 + bits(7);
            }
            if (i + repeat > nlen + ndist) throw std::runtime_error("PNG: too many code lengths");
            while (repeat--) lengths[i++] = value;
        }
        codes(build(lengths, nlen), build(lengths + nlen, ndist));
    }

    const std::vector<std::uint8_t>& in_;
    std::size_t pos_ = 0;
    unsigned cur_ = 0;
    unsigned bitpos_ = 0;
    std::vector<std::uint8_t> out_;
};

}  // namespace

void write_png(const std::filesystem::path& path, std::span<const std::uint16_t> rgb555, int width, int height) {
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < width; ++x) {
            const std::uint16_t v = rgb555[static_cast<std::size_t>(y * width + x)];
            for (int shift = 0; shift < 15; shift += 5) {
                const unsigned c = (v >> shift) & 31u;
                raw.push_back(static_cast<std::uint8_t>((c << 3) | (c >> 2)));
            }
        }
    }
    // zlib stream of stored blocks.
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::size_t pos = 0;
    do {
        const std::size_t len = std::min<std::size_t>(65535, raw.size() - pos);
        z.push_back(pos + len == raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(len));
        z.push_back(static_cast<std::uint8_t>(len >> 8));
        z.push_back(static_cast<std::uint8_t>(~len));
        z.push_back(static_cast<std::uint8_t>(~len >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
    } while (pos < raw.size());
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, static_cast<std::uint32_t>(width));
    put32(ihdr, static_cast<std::uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot write " + path.string());
    }
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

std::vector<std::uint16_t> read_png_rgb555(const std::filesystem::path& path, int width, int height) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot open " + path.string());
    }
    const std::vector<std::uint8_t> d{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (d.size() < 8 || d[1] != 'P' || d[2] != 'N' || d[3] != 'G') {
        throw std::runtime_error(path.string() + " is not a PNG");
    }
    std::vector<std::uint8_t> idat;
    for (std::size_t pos = 8; pos + 12 <= d.size();) {
        const std::uint32_t len = get32(d, pos);
        const std::string tag(d.begin() + static_cast<std::ptrdiff_t>(pos + 4), d.begin() + static_cast<std::ptrdiff_t>(pos + 8));
        if (pos + 12 + len > d.size()) throw std::runtime_error(path.string() + ": truncated chunk");
        const auto body = d.begin() + static_cast<std::ptrdiff_t>(pos + 8);
        if (tag == "IHDR") {
            if (get32(d, pos + 8) != static_cast<std::uint32_t>(width) || get32(d, pos + 12) != static_cast<std::uint32_t>(height) ||
                d[pos + 16] != 8 || d[pos + 17] != 2 || d[pos + 20] != 0) {
                throw std::runtime_error(path.string() + ": expected a non-interlaced 8-bit RGB image of the screen size");
            }
        } else if (tag == "IDAT") {
            idat.insert(idat.end(), body, body + len);
        }
        pos += 12 + len;
    }
    const std::vector<std::uint8_t> raw = Inflater(idat).run();
    const std::size_t stride = static_cast<std::size_t>(width) * 3;
    if (raw.size() != (stride + 1) * static_cast<std::size_t>(height)) {
        throw std::runtime_error(path.string() + ": unexpected image data size");
    }
    std::vector<std::uint8_t> prev(stride, 0), row(stride);
    std::vector<std::uint16_t> out;
    out.reserve(static_cast<std::size_t>(width * height));
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = raw.data() + static_cast<std::size_t>(y) * (stride + 1);
        const unsigned filter = src[0];
        for (std::size_t i = 0; i < stride; ++i) {
            const unsigned left = i >= 3 ? row[i - 3] : 0;
            const unsigned up = prev[i];
            const unsigned up_left = i >= 3 ? prev[i - 3] : 0;
            unsigned pred = 0;
            switch (filter) {
            case 0: pred = 0; break;
            case 1: pred = left; break;
            case 2: pred = up; break;
            case 3: pred = (left + up) / 2; break;
            case 4: {
                const int p = static_cast<int>(left + up) - static_cast<int>(up_left);
                const int pa = std::abs(p - static_cast<int>(left));
                const int pb = std::abs(p - static_cast<int>(up));
                const int pc = std::abs(p - static_cast<int>(up_left));
                pred = (pa <= pb && pa <= pc) ? left : (pb <= pc ? up : up_left);
                break;
            }
            default: throw std::runtime_error(path.string() + ": bad PNG filter");
            }
            row[i] = static_cast<std::uint8_t>(src[1 + i] + pred);
        }
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* p = row.data() + static_cast<std::size_t>(x) * 3;
            out.push_back(static_cast<std::uint16_t>((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10)));
        }
        prev = row;
    }
    return out;
}

}  // namespace anygba::test
