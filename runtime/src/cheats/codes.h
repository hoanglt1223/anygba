#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Cheat code formats (GBATEK "GBA Cheat Codes"), decoded into one small instruction list the
// engine runs every frame. Decryption is reimplemented from GBATEK's descriptions.
namespace anygba::cheats {

enum class Format : std::uint8_t {
    Auto,        // decided from the code's shape and which decoding makes sense
    // "AAAAAAAA:VV", ":VVVV" or ":VVVVVVVV" (also with a space): plain writes. AnyGBA also
    // accepts a pointer target "[PPPPPPPP]+OFFSET:VALUE" (written at the 32-bit pointer plus
    // OFFSET, skipped while the pointer is 0) and a value XORed with memory "VALUE^TARGET", for
    // games that encrypt values with a per-save key. "TARGET|VALUE" sets the value's bits and keeps
    // the others (flags that share a byte).
    Raw,
    GameShark,   // GameShark / Action Replay v1 and v2, encrypted (as printed)
    GameSharkRaw,
    ActionReplay,  // Pro Action Replay v3 / GameShark SP, encrypted (as printed)
    ActionReplayRaw,
    CodeBreaker,  // CodeBreaker / Xploder, plain or encrypted after a 9xxxxxxx code
};

// Names: auto, raw, gsv1, gsv1raw, arv3, arv3raw, cb.
std::optional<Format> parse_format(std::string_view name);
const char* format_name(Format format);
// For menus and messages: "GameShark v1/v2" ...
const char* format_label(Format format);

class CodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Op {
    enum class Kind : std::uint8_t {
        Write,       // [addr] = value, `count` times, addr += addr_step, value += value_step
        Add,         // [addr] += value
        Or,          // [addr] |= value
        And,         // [addr] &= value
        WriteBytes,  // bytes[] to addr..
        Indirect,    // [[addr] + offset] = value (offset in addr_step)
        If,          // condition; see skip/block/stop
        Else,
        EndIf,
        End,         // stop running this cheat for this frame
        RomPatch,    // ROM halfword at addr = value while the cheat is enabled
    };
    enum class Cond : std::uint8_t { Eq, Ne, SignedLt, SignedGt, UnsignedLt, UnsignedGt, And, Never };
    enum class Source : std::uint8_t {
        Memory,       // [addr] of `size` bytes
        KeysPressed,  // GBA buttons held: true when all of `value` are pressed
        CheatButton,  // the cheat device's own button (a hotkey)
    };

    Kind kind = Kind::Write;
    std::uint8_t size = 1;  // 1, 2 or 4 bytes
    std::uint32_t addr = 0;
    std::uint32_t value = 0;
    std::uint32_t count = 1;
    std::uint32_t addr_step = 0;
    std::uint32_t value_step = 0;
    std::vector<std::uint8_t> bytes;

    // Raw extensions (Write and Or): the target is [pointer] + addr when pointer != 0; Write's
    // value is XORed with the `size` bytes at xor_addr (or [xor_pointer] + xor_addr).
    std::uint32_t pointer = 0;
    bool xor_value = false;
    std::uint32_t xor_pointer = 0;
    std::uint32_t xor_addr = 0;

    // If
    Cond cond = Cond::Eq;
    Source source = Source::Memory;
    std::uint32_t skip = 1;     // ops skipped when false (not block)
    bool block = false;         // false: skip to the matching Else/EndIf
    bool stop_if_false = false; // false: stop the whole cheat
};

struct Program {
    Format format = Format::Raw;  // the format actually decoded (Auto resolved)
    std::vector<Op> ops;
    bool has_rom_patch = false;
};

// One code line per entry. Hex digits may be split by spaces, ':', '+' or '-'. Throws CodeError
// naming the line for malformed or unsupported codes (DEADFACE reseeding, unknown types).
Program compile(Format format, std::span<const std::string> lines);

// Splits text the way users type or paste codes: newlines, ';', ',' and libretro's '+'
// between whole code words. Empty lines are dropped.
std::vector<std::string> split_lines(std::string_view text);

// Exposed for tests: the decryption steps.
void gameshark_decrypt(std::uint32_t& addr, std::uint32_t& value, bool action_replay_v3);

class CodeBreakerCipher {
public:
    // Activated by a "9xyyxxxx xxxx" code.
    void set_key(std::uint32_t addr, std::uint16_t value);
    bool active() const { return active_; }
    void decrypt(std::uint32_t& addr, std::uint16_t& value) const;

private:
    bool active_ = false;
    std::uint8_t swap_[48] = {};
    std::uint8_t seed_[14] = {};
};

}  // namespace anygba::cheats
