#pragma once

#include "anygba/cpu.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The override registry and its startup checks (docs/dev/OVERRIDES.md). Override modules register
// implementations by name (anygba/override.hpp); the generated program lists which guest
// functions use which name; activate() connects the two.
namespace anygba::overrides {

// on: the overrides enabled in the config; off: none (A/B comparison); all: also the disabled ones.
enum class Mode { On, Off, All };
std::optional<Mode> parse_mode(std::string_view text);

struct Active {
    const anygba_override_entry* entry = nullptr;
    bool on = false;
};

// Checks that every override of `game` has a registered implementation, then switches them on
// according to `mode`. Throws anygba::Error naming the override and the missing or duplicated
// implementation. Overrides are process-wide, like the generated code they belong to.
std::vector<Active> activate(const anygba_game& game, Mode mode);

// "StringCopy at 0x08008ba0 thumb -> pokemon_gen3_StringCopy [pokemon-gen3]: on"; an override
// given by address starts "0x08008ba0 thumb".
std::string describe(const Active& active);

// Where Guest::osd() messages go; empty: dropped.
void set_osd_sink(std::function<void(const std::string&)> sink);

}  // namespace anygba::overrides
