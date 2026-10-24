#pragma once

#include "cheats/codes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The cheats a game can use: entries from cheat databases (docs/user/CHEATS.md) and the
// user's own codes, with what is enabled. Database entries are named by category path
// ("Items/Master Ball x99"); an entry may take a value (an item, a species, a quantity)
// that is written into its code where `{v:N}` stands (N hex digits).
namespace anygba::cheats {

struct Choice {
    std::uint32_t value = 0;
    std::string name;
};

struct Param {
    std::string label;
    std::vector<Choice> choices;  // empty: any number in min..max
    std::uint32_t min = 0;
    std::uint32_t max = 0;
    std::string describe(std::uint32_t value) const;
    bool accepts(std::uint32_t value) const;
};

struct Cheat {
    std::string name;  // '/' separates categories
    Format format = Format::Auto;
    std::vector<std::string> code;
    std::string note;
    std::optional<Param> param;
    std::uint32_t value = 0;
    bool enabled = false;
    bool custom = false;      // the user's own code, not from a database
    bool persistent = true;   // written to the per-game cheat file (false: --cheat/--cheats)
    Program program;          // compiled while enabled
};

// "82025E30 {v:4}" with 0x1A -> "82025E30 001A". Throws CodeError for a malformed slot.
std::string expand(std::string_view line, std::uint32_t value);
bool has_slot(std::string_view line);

class CheatList {
public:
    // Adds a database's entries. Throws std::runtime_error naming the file and line for
    // malformed files. Returns false (and adds nothing) when the file names other ROMs.
    bool load_database(const std::filesystem::path& path, std::uint32_t rom_crc);
    // Custom cheats and database picks from the per-game cheat file (or a --cheats file:
    // `persistent` false). A missing file is not an error.
    void load_user(const std::filesystem::path& path, bool persistent = true);
    // Writes custom cheats and database picks. Returns an error message on failure.
    std::optional<std::string> save_user(const std::filesystem::path& path, std::string_view title) const;

    // Throws CodeError when the code does not compile.
    std::size_t add_custom(std::string name, Format format, std::vector<std::string> lines, bool persistent = true);
    void remove(std::size_t index);
    // Returns an error message when the code does not compile; the cheat then stays off.
    std::optional<std::string> set_enabled(std::size_t index, bool enabled);
    std::optional<std::string> set_value(std::size_t index, std::uint32_t value);

    const std::vector<Cheat>& cheats() const { return cheats_; }
    std::vector<const Program*> active() const;
    // Increments on every change; the player compares it to sync ROM patches and the file.
    std::uint64_t generation() const { return generation_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    std::optional<std::string> compile(Cheat& cheat);
    std::vector<Cheat> cheats_;
    std::vector<std::string> warnings_;
    std::uint64_t generation_ = 0;
};

}  // namespace anygba::cheats
