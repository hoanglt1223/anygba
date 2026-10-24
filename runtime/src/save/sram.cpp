#include "save/save.h"

#include <stdexcept>

namespace anygba::save {

namespace {

constexpr std::size_t kSize = 0x8000;

// 32 KB battery-backed SRAM, mirrored through the whole 0x0E/0x0F area.
class Sram final : public Backup {
public:
    Sram() { data_.assign(kSize, 0xFF); }
    Type type() const override { return Type::Sram; }
    std::uint8_t read8(std::uint32_t addr) override { return data_[addr & (kSize - 1)]; }
    void write8(std::uint32_t addr, std::uint8_t value) override {
        std::uint8_t& cell = data_[addr & (kSize - 1)];
        if (cell != value) {
            cell = value;
            changed();
        }
    }
    void load(std::vector<std::uint8_t> bytes) override {
        // Some tools write 64 KB SRAM files; the upper half is a mirror.
        if (bytes.size() != kSize && bytes.size() != 2 * kSize) {
            throw std::runtime_error("SRAM save must be 32768 bytes, got " + std::to_string(bytes.size()));
        }
        bytes.resize(kSize);
        data_ = std::move(bytes);
    }
};

}  // namespace

std::unique_ptr<Backup> make_sram() { return std::make_unique<Sram>(); }

}  // namespace anygba::save
