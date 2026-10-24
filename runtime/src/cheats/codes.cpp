#include "cheats/codes.h"

#include <array>
#include <cstdio>
#include <utility>

namespace anygba::cheats {

namespace {

using Kind = Op::Kind;
using Cond = Op::Cond;
using Source = Op::Source;

constexpr std::array<std::uint32_t, 4> kGameSharkSeeds = {0x09F4FBBD, 0x9681884A, 0x352027E9, 0xF3DEE5A7};
constexpr std::array<std::uint32_t, 4> kActionReplaySeeds = {0x7AA9648F, 0x7FAE6994, 0xC0EFAAD5, 0x42712C57};
constexpr std::uint32_t kDelta = 0x9E3779B9;
constexpr std::uint32_t kIdMarker = 0x001DC0DE;
constexpr std::uint32_t kReseed = 0xDEADFACE;

struct Line {
    std::uint32_t a = 0;  // first (address) word
    std::uint32_t v = 0;  // second word: 32 bits, 16 for CodeBreaker
    std::size_t number = 0;  // 1-based, for messages
    // Raw format only
    std::size_t value_digits = 0;
};

[[noreturn]] void fail(const Line& line, const std::string& message) {
    char head[96];
    std::snprintf(head, sizeof head, "line %zu (%08X %08X): ", line.number, line.a, line.v);
    throw CodeError(head + message);
}

bool is_hex(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }

std::uint32_t hex_value(std::string_view digits) {
    std::uint32_t v = 0;
    for (const char ch : digits) {
        const int d = ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10;
        v = (v << 4) | static_cast<std::uint32_t>(d);
    }
    return v;
}

// Hex groups of one line, e.g. "82025E30 0001" -> {"82025E30", "0001"}.
std::vector<std::string> groups(std::string_view text, std::size_t number) {
    std::vector<std::string> out;
    std::string current;
    for (const char ch : text) {
        if (is_hex(ch)) {
            current += ch;
        } else if (ch == ' ' || ch == '\t' || ch == ':' || ch == '-' || ch == '+' || ch == '\r') {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
        } else {
            throw CodeError("line " + std::to_string(number) + ": unexpected character '" + std::string(1, ch) + "'");
        }
    }
    if (!current.empty()) {
        out.push_back(std::move(current));
    }
    return out;
}

// Splits one line into an 8-digit word and a value of `value_digits` (0: any of 2/4/8).
Line split_line(const std::string& text, std::size_t number, std::size_t value_digits) {
    const std::vector<std::string> g = groups(text, number);
    std::string all;
    for (const auto& s : g) all += s;
    Line line;
    line.number = number;
    std::size_t vd = 0;
    if (g.size() == 2 && g[0].size() == 8) {
        vd = g[1].size();
    } else if (g.size() == 1 && all.size() > 8) {
        vd = all.size() - 8;
    } else {
        throw CodeError("line " + std::to_string(number) + ": expected an 8-digit address and a value, got '" + text + "'");
    }
    const bool ok = value_digits ? vd == value_digits : (vd == 2 || vd == 4 || vd == 8);
    if (!ok) {
        throw CodeError("line " + std::to_string(number) + ": expected " +
                        (value_digits ? std::to_string(value_digits) : std::string("2, 4 or 8")) +
                        " value digits, got " + std::to_string(vd) + " in '" + text + "'");
    }
    line.a = hex_value(std::string_view(all).substr(0, 8));
    line.v = hex_value(std::string_view(all).substr(8));
    line.value_digits = vd;
    return line;
}

Op write(std::uint8_t size, std::uint32_t addr, std::uint32_t value) {
    Op op;
    op.kind = Kind::Write;
    op.size = size;
    op.addr = addr;
    op.value = value;
    return op;
}

Op make(Kind kind, std::uint8_t size, std::uint32_t addr, std::uint32_t value) {
    Op op = write(size, addr, value);
    op.kind = kind;
    return op;
}

Op if_memory(Cond cond, std::uint8_t size, std::uint32_t addr, std::uint32_t value, std::uint32_t skip) {
    Op op = make(Kind::If, size, addr, value);
    op.cond = cond;
    op.skip = skip;
    return op;
}

Op if_source(Source source, std::uint32_t value) {
    Op op = make(Kind::If, 2, 0, value);
    op.source = source;
    return op;
}

Op rom_patch(std::uint32_t halfword_index, std::uint32_t value) {
    return make(Kind::RomPatch, 2, 0x08000000u + ((halfword_index & 0x00FFFFFF) << 1), value & 0xFFFF);
}

// ---- GameShark v1/v2 ----

void decode_gameshark(const std::vector<Line>& lines, Program& p) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Line& l = lines[i];
        if (l.a == kReseed) {
            fail(l, "DEADFACE reseeding is not supported (its tables are not public)");
        }
        if (l.v == kIdMarker) {
            continue;  // game ID enable code
        }
        const std::uint32_t addr = l.a & 0x0FFFFFFF;
        switch (l.a >> 28) {
        case 0x0:
            if (l.v > 0xFF) fail(l, "8-bit write with a value above FF");
            p.ops.push_back(write(1, addr, l.v));
            break;
        case 0x1:
            if (l.v > 0xFFFF) fail(l, "16-bit write with a value above FFFF");
            p.ops.push_back(write(2, addr, l.v));
            break;
        case 0x2: p.ops.push_back(write(4, addr, l.v)); break;
        case 0x3: {
            if ((l.a & 0x0FFF0000) != 0) fail(l, "unknown 3-type code");
            const std::uint32_t count = l.a & 0xFFFF;
            std::uint32_t written = 0;
            while (written < count) {
                if (++i >= lines.size()) fail(l, "address list ends early");
                for (const std::uint32_t target : {lines[i].a, lines[i].v}) {
                    if (written < count && target != 0) {
                        p.ops.push_back(write(4, target, l.v));
                    }
                    ++written;
                }
            }
            break;
        }
        case 0x6:
            if ((l.a & 0x0F000000) != 0 || (l.v & 0x0FFF0000) != 0) fail(l, "malformed ROM patch");
            p.ops.push_back(rom_patch(l.a, l.v));
            p.has_rom_patch = true;
            break;
        case 0x8: {
            if (l.a == 0x80F00000) {
                break;  // slowdown: meaningless here
            }
            const std::uint32_t kind = (l.a >> 20) & 0xF;
            const std::uint32_t target = ((l.a & 0x0F000000)) | (l.a & 0x000FFFFF);
            if (kind != 1 && kind != 2) fail(l, "unknown 8-type code");
            p.ops.push_back(if_source(Source::CheatButton, 0));
            p.ops.push_back(write(kind == 1 ? 1 : 2, target, l.v & (kind == 1 ? 0xFFu : 0xFFFFu)));
            break;
        }
        case 0xD:
            if (l.v > 0xFFFF) fail(l, "malformed D-type condition");
            p.ops.push_back(if_memory(Cond::Eq, 2, addr, l.v, 1));
            break;
        case 0xE:
            if ((l.a & 0x0F000000) != 0 || (l.v & 0xF0000000) != 0) fail(l, "malformed E-type condition");
            p.ops.push_back(if_memory(Cond::Eq, 2, l.v & 0x0FFFFFFF, l.a & 0xFFFF, (l.a >> 16) & 0xFF));
            break;
        case 0xF:
            if ((l.v & 0xFFFFF0F0) != 0 || addr < 0x08000000 || addr >= 0x0A000000) fail(l, "malformed hook code");
            break;  // hook enable code
        default: fail(l, "unknown GameShark code type");
        }
    }
}

// ---- Pro Action Replay v3 ----

std::uint32_t ar_address(std::uint32_t a) { return ((a << 4) & 0x0F000000) | (a & 0x000FFFFF); }

void decode_action_replay(const std::vector<Line>& lines, Program& p) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Line& l = lines[i];
        if (l.a == kReseed) {
            fail(l, "DEADFACE reseeding is not supported (its tables are not public)");
        }
        if (l.v == kIdMarker) {
            continue;
        }
        const auto next = [&]() -> const Line& {
            if (++i >= lines.size()) fail(l, "code needs a second line");
            return lines[i];
        };
        if (l.a == 0) {
            const std::uint32_t type = l.v >> 24;
            const std::uint32_t target = ar_address(l.v);
            if (l.v == 0) {
                p.ops.push_back(make(Kind::End, 1, 0, 0));
            } else if (l.v == 0x60000000) {
                p.ops.push_back(make(Kind::Else, 1, 0, 0));
            } else if (l.v == 0x40000000) {
                p.ops.push_back(make(Kind::EndIf, 1, 0, 0));
            } else if ((l.v & 0xFFFF00FF) == 0x08000000) {
                // slowdown
            } else if (type == 0x10 || type == 0x12 || type == 0x14) {
                const Line& arg = next();
                const std::uint8_t size = type == 0x10 ? 1 : type == 0x12 ? 2 : 4;
                p.ops.push_back(if_source(Source::CheatButton, 0));
                p.ops.push_back(write(size, target, size == 4 ? arg.a : arg.a & (size == 1 ? 0xFFu : 0xFFFFu)));
            } else if (type == 0x18 || type == 0x1A || type == 0x1C || type == 0x1E) {
                const Line& arg = next();
                if (arg.a > 0xFFFF) fail(arg, "malformed ROM patch value");
                p.ops.push_back(rom_patch(l.v, arg.a));
                p.has_rom_patch = true;
            } else if (type == 0x80 || type == 0x82 || type == 0x84) {
                const Line& arg = next();
                const std::uint8_t size = type == 0x80 ? 1 : type == 0x82 ? 2 : 4;
                Op op = write(size, target, size == 4 ? arg.a : arg.a & (size == 1 ? 0xFFu : 0xFFFFu));
                op.value_step = arg.v >> 24;
                op.count = (arg.v >> 16) & 0xFF;
                op.addr_step = (arg.v & 0xFFFF) * size;
                p.ops.push_back(op);
            } else {
                fail(l, "unknown Action Replay special code");
            }
            continue;
        }
        const std::uint32_t sub = l.a >> 30;
        const std::uint32_t typ = (l.a >> 27) & 7;
        const std::uint32_t siz = (l.a >> 25) & 3;
        const std::uint32_t xtr = (l.a >> 24) & 1;
        const std::uint32_t addr = ar_address(l.a);
        if ((l.a & 0x00C00000) != 0) {
            fail(l, "unused address bits are set");
        }
        if (typ != 0) {
            static constexpr Cond kConds[] = {Cond::Eq,         Cond::Eq,         Cond::Ne,         Cond::SignedLt,
                                              Cond::SignedGt,   Cond::UnsignedLt, Cond::UnsignedGt, Cond::And};
            const std::uint8_t size = siz == 0 ? 1 : siz == 1 ? 2 : 4;
            const std::uint32_t mask = siz == 0 ? 0xFFu : siz == 1 ? 0xFFFFu : 0xFFFFFFFFu;
            if (siz < 3 && (l.v & ~mask) != 0) fail(l, "condition value wider than its size");
            Op op = if_memory(siz == 3 ? Cond::Never : kConds[typ], size, addr, l.v, sub == 1 ? 2 : 1);
            op.block = sub == 2;
            op.stop_if_false = sub == 3;
            p.ops.push_back(op);
            continue;
        }
        if (xtr && !(sub == 3 && siz == 3)) {
            fail(l, "unknown Action Replay code type");
        }
        switch (sub) {
        case 0:  // write, with a fill count for 8 and 16 bits
            if (siz == 0) {
                Op op = write(1, addr, l.v & 0xFF);
                op.count = (l.v >> 8) + 1;
                op.addr_step = 1;
                p.ops.push_back(op);
            } else if (siz == 1) {
                Op op = write(2, addr, l.v & 0xFFFF);
                op.count = (l.v >> 16) + 1;
                op.addr_step = 2;
                p.ops.push_back(op);
            } else if (siz == 2) {
                p.ops.push_back(write(4, addr, l.v));
            } else {
                fail(l, "unknown Action Replay write size");
            }
            break;
        case 1: {  // indirect
            if (siz == 3) fail(l, "unknown Action Replay pointer size");
            Op op = make(Kind::Indirect, siz == 0 ? 1 : siz == 1 ? 2 : 4, addr,
                         siz == 0 ? l.v & 0xFF : siz == 1 ? l.v & 0xFFFF : l.v);
            op.addr_step = siz == 0 ? l.v >> 8 : siz == 1 ? (l.v >> 16) * 2 : 0;
            p.ops.push_back(op);
            break;
        }
        case 2:  // add
            if (siz == 3) fail(l, "unknown Action Replay add size");
            if (siz < 2 && (l.v >> (siz == 0 ? 8 : 16)) != 0) fail(l, "add value wider than its size");
            p.ops.push_back(make(Kind::Add, siz == 0 ? 1 : siz == 1 ? 2 : 4, addr, l.v));
            break;
        default:  // sub 3: hook (C4) and I/O writes (C6, C7)
            if (siz == 2 && !xtr) {
                if ((l.v >> 16) != 0) fail(l, "malformed hook code");
                break;
            }
            if (siz == 3) {
                const std::uint32_t io = 0x04000000u | (l.a & 0x00FFFFFF);
                if (!xtr && l.v > 0xFFFF) fail(l, "16-bit I/O write with a value above FFFF");
                p.ops.push_back(write(xtr ? 4 : 2, io, l.v));
                break;
            }
            fail(l, "unknown Action Replay code type");
        }
    }
}

// ---- CodeBreaker ----

void decode_codebreaker(const std::vector<Line>& lines, Program& p) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Line& l = lines[i];
        const std::uint32_t addr = l.a & 0x0FFFFFFF;
        const auto next = [&]() -> const Line& {
            if (++i >= lines.size()) fail(l, "code needs another line");
            return lines[i];
        };
        switch (l.a >> 28) {
        case 0x0: break;  // game ID
        case 0x1: break;  // hook
        case 0x2: p.ops.push_back(make(Kind::Or, 2, addr, l.v)); break;
        case 0x3:
            if (l.v > 0xFF) fail(l, "8-bit write with a value above FF");
            p.ops.push_back(write(1, addr, l.v));
            break;
        case 0x4: {
            const Line& arg = next();
            Op op = write(2, addr, l.v);
            op.value_step = arg.a >> 16;
            op.count = arg.a & 0xFFFF;
            op.addr_step = arg.v;
            p.ops.push_back(op);
            break;
        }
        case 0x5: {
            Op op = make(Kind::WriteBytes, 1, addr, 0);
            const std::uint32_t count = l.v;
            while (op.bytes.size() < count) {
                const Line& data = next();
                const std::uint8_t six[6] = {static_cast<std::uint8_t>(data.a >> 24), static_cast<std::uint8_t>(data.a >> 16),
                                             static_cast<std::uint8_t>(data.a >> 8),  static_cast<std::uint8_t>(data.a),
                                             static_cast<std::uint8_t>(data.v >> 8),  static_cast<std::uint8_t>(data.v)};
                for (const std::uint8_t b : six) {
                    if (op.bytes.size() < count) op.bytes.push_back(b);
                }
            }
            p.ops.push_back(std::move(op));
            break;
        }
        case 0x6: p.ops.push_back(make(Kind::And, 2, addr, l.v)); break;
        case 0x7: p.ops.push_back(if_memory(Cond::Eq, 2, addr, l.v, 1)); break;
        case 0x8: p.ops.push_back(write(2, addr, l.v)); break;
        case 0x9: break;  // encryption key, applied while reading the lines
        case 0xA: p.ops.push_back(if_memory(Cond::Ne, 2, addr, l.v, 1)); break;
        case 0xB: p.ops.push_back(if_memory(Cond::SignedGt, 2, addr, l.v, 1)); break;
        case 0xC: p.ops.push_back(if_memory(Cond::SignedLt, 2, addr, l.v, 1)); break;
        case 0xD:
            if (addr != 0x20) fail(l, "unknown D-type code (only D0000020 joypad conditions exist)");
            p.ops.push_back(if_source(Source::KeysPressed, l.v & 0x3FF));
            break;
        case 0xE: p.ops.push_back(make(Kind::Add, 2, addr, l.v)); break;
        case 0xF: p.ops.push_back(if_memory(Cond::And, 2, addr, l.v, 1)); break;
        }
    }
}

std::vector<Line> read_lines(std::span<const std::string> text, std::size_t value_digits) {
    std::vector<Line> lines;
    for (std::size_t i = 0; i < text.size(); ++i) {
        lines.push_back(split_line(text[i], i + 1, value_digits));
    }
    if (lines.empty()) {
        throw CodeError("the code is empty");
    }
    return lines;
}

Program compile_known(Format format, std::span<const std::string> text) {
    Program p;
    p.format = format;
    switch (format) {
    case Format::Raw:
        for (const Line& l : read_lines(text, 0)) {
            p.ops.push_back(write(static_cast<std::uint8_t>(l.value_digits / 2), l.a, l.v));
        }
        break;
    case Format::GameShark:
    case Format::GameSharkRaw:
    case Format::ActionReplay:
    case Format::ActionReplayRaw: {
        std::vector<Line> lines = read_lines(text, 8);
        const bool ar = format == Format::ActionReplay || format == Format::ActionReplayRaw;
        if (format == Format::GameShark || format == Format::ActionReplay) {
            for (Line& l : lines) gameshark_decrypt(l.a, l.v, ar);
        }
        ar ? decode_action_replay(lines, p) : decode_gameshark(lines, p);
        break;
    }
    case Format::CodeBreaker: {
        std::vector<Line> lines = read_lines(text, 4);
        CodeBreakerCipher cipher;
        for (Line& l : lines) {
            if (cipher.active()) {
                auto v = static_cast<std::uint16_t>(l.v);
                cipher.decrypt(l.a, v);
                l.v = v;
            }
            if ((l.a >> 28) == 0x9) {
                cipher.set_key(l.a, static_cast<std::uint16_t>(l.v));
            }
        }
        decode_codebreaker(lines, p);
        break;
    }
    case Format::Auto: break;
    }
    return p;
}

bool plausible_ram(std::uint32_t addr, std::uint32_t size) {
    const auto in = [&](std::uint32_t base, std::uint32_t len) { return addr >= base && addr + size <= base + len; };
    return in(0x02000000, 0x40000) || in(0x03000000, 0x8000) || in(0x04000000, 0x400) || in(0x05000000, 0x400) ||
           in(0x06000000, 0x18000) || in(0x07000000, 0x400);
}

// Whether a decoding of a 16-digit code looks like something a person wrote: every access
// lands in RAM, I/O or video memory, and ROM patches inside a cartridge.
bool plausible(const Program& p) {
    for (const Op& op : p.ops) {
        switch (op.kind) {
        case Kind::Write:
        case Kind::Add:
        case Kind::Or:
        case Kind::And:
        case Kind::Indirect:
            if (!plausible_ram(op.addr, op.size) || op.count == 0 || op.count > 0x10000) return false;
            break;
        case Kind::If:
            if (op.source == Source::Memory && op.cond != Cond::Never && !plausible_ram(op.addr, op.size)) return false;
            break;
        case Kind::RomPatch:
            if (op.addr >= 0x0A000000) return false;
            break;
        default: break;
        }
    }
    return true;
}

std::size_t digit_count(const std::string& line) {
    std::size_t n = 0;
    for (const char ch : line) n += is_hex(ch) ? 1 : 0;
    return n;
}

}  // namespace

std::optional<Format> parse_format(std::string_view name) {
    static constexpr std::pair<std::string_view, Format> kNames[] = {
        {"auto", Format::Auto},         {"raw", Format::Raw},          {"gsv1", Format::GameShark},
        {"gsv1raw", Format::GameSharkRaw}, {"arv3", Format::ActionReplay}, {"arv3raw", Format::ActionReplayRaw},
        {"cb", Format::CodeBreaker},
    };
    for (const auto& [n, f] : kNames) {
        if (n == name) return f;
    }
    return std::nullopt;
}

const char* format_name(Format format) {
    switch (format) {
    case Format::Auto: return "auto";
    case Format::Raw: return "raw";
    case Format::GameShark: return "gsv1";
    case Format::GameSharkRaw: return "gsv1raw";
    case Format::ActionReplay: return "arv3";
    case Format::ActionReplayRaw: return "arv3raw";
    case Format::CodeBreaker: return "cb";
    }
    return "?";
}

const char* format_label(Format format) {
    switch (format) {
    case Format::Auto: return "Auto-detect";
    case Format::Raw: return "Raw address:value";
    case Format::GameShark: return "GameShark v1/v2";
    case Format::GameSharkRaw: return "GameShark v1/v2 (decrypted)";
    case Format::ActionReplay: return "Action Replay v3";
    case Format::ActionReplayRaw: return "Action Replay v3 (decrypted)";
    case Format::CodeBreaker: return "CodeBreaker";
    }
    return "?";
}

void gameshark_decrypt(std::uint32_t& addr, std::uint32_t& value, bool action_replay_v3) {
    const auto& s = action_replay_v3 ? kActionReplaySeeds : kGameSharkSeeds;
    std::uint32_t sum = kDelta * 32;
    for (int i = 0; i < 32; ++i) {
        value -= ((addr << 4) + s[2]) ^ (addr + sum) ^ ((addr >> 5) + s[3]);
        addr -= ((value << 4) + s[0]) ^ (value + sum) ^ ((value >> 5) + s[1]);
        sum -= kDelta;
    }
}

namespace {

struct CbRandom {
    std::uint32_t state = 0;
    std::uint32_t step() { return state = state * 0x41C64E6Du + 0x3039u; }
    std::uint32_t next() {
        std::uint32_t x = (step() << 14) & 0xC0000000u;
        x += (step() >> 1) & 0x3FFF8000u;
        x += (step() >> 16) & 0x00007FFFu;
        return x;
    }
};

void put16(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}
std::uint32_t get16(const std::uint8_t* p) { return p[0] | (p[1] << 8); }
std::uint32_t get32(const std::uint8_t* p) { return get16(p) | (get16(p + 2) << 16); }

}  // namespace

void CodeBreakerCipher::set_key(std::uint32_t addr, std::uint16_t value) {
    std::uint8_t code[6];
    put32(code, addr);
    put16(code + 4, value);
    for (std::uint8_t i = 0; i < 48; ++i) swap_[i] = i;
    CbRandom r;
    r.state = 0x1111u ^ code[4];
    for (int i = 0; i < 0x50; ++i) {
        const std::uint32_t a = r.next() % 48;
        const std::uint32_t b = r.next() % 48;
        std::swap(swap_[a], swap_[b]);
    }
    put16(seed_, get16(code));
    r.state = 0x4EFAD1C3u;
    for (int i = 0; i <= static_cast<int>(code[3]) - 0x91; ++i) r.state = r.next();
    put32(seed_ + 2, r.next());
    put16(seed_ + 6, r.next());
    r.state = 0xF254u ^ code[5];
    for (int i = 0; i < code[5]; ++i) r.state = r.next();
    put32(seed_ + 8, r.next());
    put16(seed_ + 12, r.next());
    active_ = true;
}

void CodeBreakerCipher::decrypt(std::uint32_t& addr, std::uint16_t& value) const {
    static constexpr std::uint8_t kXlat[6] = {3, 2, 1, 0, 5, 4};
    std::uint8_t code[6];
    put32(code, addr);
    put16(code + 4, value);
    for (int i = 47; i >= 0; --i) {
        const int j = swap_[i];
        const int bit1 = i & 7, index1 = kXlat[i / 8];
        const int bit2 = j & 7, index2 = kXlat[j / 8];
        const int b1 = (code[index1] >> bit1) & 1;
        const int b2 = (code[index2] >> bit2) & 1;
        code[index1] = static_cast<std::uint8_t>((code[index1] & ~(1 << bit1)) | (b2 << bit1));
        code[index2] = static_cast<std::uint8_t>((code[index2] & ~(1 << bit2)) | (b1 << bit2));
    }
    put32(code, get32(code) ^ get32(seed_ + 8));
    std::uint32_t i = code[3] * 0x01010000u + code[0] * 0x100u + code[5];
    i = (get16(code + 1) * 0x10001u) ^ get32(seed_ + 2) ^ i;
    i = (seed_[0] * 0x01010101u) ^ (seed_[1] * 0x01000000u) ^ i;
    std::uint32_t j = code[5] + ((code[0] ^ code[4]) << 8);
    j = (seed_[0] * 0x101u) ^ get16(seed_ + 6) ^ j;
    addr = i;
    value = static_cast<std::uint16_t>(j);
}

Program compile(Format format, std::span<const std::string> lines) {
    if (format != Format::Auto) {
        return compile_known(format, lines);
    }
    if (lines.empty()) {
        throw CodeError("the code is empty");
    }
    bool colon = false;
    bool all12 = true;
    bool all16 = true;
    for (const std::string& line : lines) {
        colon = colon || line.find(':') != std::string::npos;
        const std::size_t n = digit_count(line);
        all12 = all12 && n == 12;
        all16 = all16 && n == 16;
    }
    if (colon) {
        return compile_known(Format::Raw, lines);
    }
    if (all12) {
        return compile_known(Format::CodeBreaker, lines);
    }
    if (!all16) {
        return compile_known(Format::Raw, lines);
    }
    std::string reasons;
    for (const Format f : {Format::ActionReplay, Format::GameShark, Format::ActionReplayRaw, Format::GameSharkRaw}) {
        try {
            Program p = compile_known(f, lines);
            if (plausible(p)) {
                return p;
            }
            reasons += std::string("; ") + format_label(f) + ": addresses outside RAM";
        } catch (const CodeError& e) {
            reasons += std::string("; ") + format_label(f) + ": " + e.what();
        }
    }
    throw CodeError("could not tell which device this code is for" + reasons);
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> words;
    std::string current;
    const auto flush = [&] {
        std::size_t first = current.find_first_not_of(" \t\r");
        if (first != std::string::npos) {
            const std::size_t last = current.find_last_not_of(" \t\r");
            words.push_back(current.substr(first, last - first + 1));
        }
        current.clear();
    };
    for (const char ch : text) {
        if (ch == '\n' || ch == ';' || ch == ',') {
            flush();
        } else {
            current += ch;
        }
    }
    flush();
    // libretro joins every word with '+': "AAAAAAAA+VVVV+AAAAAAAA+VVVV". Pair the words back
    // into lines when a line holds nothing but '+'-joined hex words.
    std::vector<std::string> lines;
    for (const std::string& w : words) {
        if (w.find('+') == std::string::npos) {
            lines.push_back(w);
            continue;
        }
        std::vector<std::string> parts;
        std::string part;
        for (const char ch : w) {
            if (ch == '+') {
                parts.push_back(part);
                part.clear();
            } else if (ch != ' ') {
                part += ch;
            }
        }
        parts.push_back(part);
        for (std::size_t i = 0; i < parts.size(); i += 2) {
            lines.push_back(i + 1 < parts.size() ? parts[i] + " " + parts[i + 1] : parts[i]);
        }
    }
    return lines;
}

}  // namespace anygba::cheats
