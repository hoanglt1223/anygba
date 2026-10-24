#pragma once

#include "control/json.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Per-game named memory (docs/dev/GAME-CONFIG.md "[symbols]"): agents read `money` or
// `party[0].hp` instead of raw bytes. Read from the [symbols] table of a game TOML or of a
// symbols file such as compat/symbols/BPEE.toml; every other table is ignored.
namespace anygba::control {

enum class SymType : std::uint8_t { U8, U16, U32, S8, S16, S32, Bool, Bytes, Text, Struct };

struct SymField {
    std::string name;
    std::uint32_t offset = 0;
    SymType type = SymType::U8;
    std::uint32_t size = 1;   // bytes per element
    std::uint32_t count = 1;  // >1: an array
    int bit = -1;             // >=0: this bit of the value, as a boolean
    std::string xor_with;     // symbol whose value is XORed into this one
    std::string charset;      // text: "ascii" or "pokemon"
};

struct Symbol : SymField {
    std::optional<std::uint32_t> pointer;  // address of a 32-bit pointer; `offset` is relative to it
    std::vector<SymField> fields;          // struct members
    bool journal = false;                  // part of the journal's periodic snapshots
    std::string description;
};

// Reads guest memory into `out`; throws for unreadable addresses.
using Peek = std::function<void(std::uint32_t addr, std::span<std::uint8_t> out)>;

class Symbols {
public:
    // Throws std::runtime_error naming the source, the symbol and the problem.
    static Symbols parse(std::string_view toml_text, std::string_view source_name);
    static Symbols load(const std::filesystem::path& path);
    // Adds `other`'s symbols; a name defined in both takes `other`'s definition.
    void merge(const Symbols& other);

    bool empty() const { return symbols_.empty(); }
    const std::vector<Symbol>& all() const { return symbols_; }
    const Symbol* find(std::string_view name) const;

    // `name`, `name[i]`, `name[i].field`, `name.field`, `name[i].field[j]`. Throws
    // std::runtime_error for unknown names, bad indexes and unreadable memory.
    Json read(std::string_view path, const Peek& peek) const;
    // Name, type, resolved address and layout of every symbol, for agents.
    Json describe(const Peek& peek) const;
    // The values of every `journal = true` symbol; unreadable ones as {"error": ...}.
    Json snapshot(const Peek& peek) const;

private:
    std::uint32_t base(const Symbol& s, const Peek& peek) const;
    Json read_value(const SymField& f, std::uint32_t addr, const Peek& peek) const;
    Json read_element(const Symbol& s, std::uint32_t addr, const Peek& peek) const;
    std::uint32_t xor_key(const std::string& name, const Peek& peek, int depth) const;
    std::vector<Symbol> symbols_;
};

std::string decode_text(std::span<const std::uint8_t> bytes, std::string_view charset);

}  // namespace anygba::control
