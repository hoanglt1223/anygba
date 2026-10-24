#include "save/save.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace anygba::save {

namespace {

constexpr std::pair<std::string_view, Type> kNames[] = {
    {"none", Type::None},         {"sram", Type::Sram},         {"flash64", Type::Flash64},
    {"flash128", Type::Flash128}, {"eeprom", Type::Eeprom},     {"eeprom512", Type::Eeprom512},
    {"eeprom8k", Type::Eeprom8k},
};

bool contains(std::span<const std::uint8_t> rom, std::string_view needle) {
    return std::search(rom.begin(), rom.end(), needle.begin(), needle.end()) != rom.end();
}

class NoBackup final : public Backup {
public:
    Type type() const override { return Type::None; }
    std::uint8_t read8(std::uint32_t) override { return 0xFF; }
    void write8(std::uint32_t, std::uint8_t) override {}
    void load(std::vector<std::uint8_t> bytes) override {
        if (!bytes.empty()) {
            throw std::runtime_error("this game has no backup memory, but the save file holds " +
                                     std::to_string(bytes.size()) + " bytes");
        }
    }
};

}  // namespace

Type detect(std::span<const std::uint8_t> rom) {
    // The IDs are word-aligned in real ROMs, but test ROMs and hacks are not always; a plain
    // search is cheap enough. Longer names first: "FLASH_V" is not a prefix of the others,
    // but "SRAM_V" and "SRAM_F_V" both mean SRAM.
    if (contains(rom, "EEPROM_V")) return Type::Eeprom;
    if (contains(rom, "FLASH1M_V")) return Type::Flash128;
    if (contains(rom, "FLASH512_V") || contains(rom, "FLASH_V")) return Type::Flash64;
    if (contains(rom, "SRAM_V") || contains(rom, "SRAM_F_V")) return Type::Sram;
    return Type::None;
}

std::optional<Type> parse(std::string_view text) {
    for (const auto& [n, t] : kNames) {
        if (n == text) return t;
    }
    return std::nullopt;
}

const char* name(Type type) {
    for (const auto& [n, t] : kNames) {
        if (t == type) return n.data();
    }
    return "?";
}

std::unique_ptr<Backup> make_none() { return std::make_unique<NoBackup>(); }

std::unique_ptr<Backup> make(Type type) {
    switch (type) {
    case Type::None: return make_none();
    case Type::Sram: return make_sram();
    case Type::Flash64: return make_flash(false);
    case Type::Flash128: return make_flash(true);
    case Type::Eeprom: return make_eeprom(0);
    case Type::Eeprom512: return make_eeprom(512);
    case Type::Eeprom8k: return make_eeprom(8192);
    }
    throw std::invalid_argument("unknown save type");
}

}  // namespace anygba::save
