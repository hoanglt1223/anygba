#include "save/save.h"

#include <stdexcept>

// Serial EEPROM (GBATEK "GBA Cart Backup EEPROM"), driven one bit per halfword, normally by
// DMA 3. A request is "11" + address + "0" (read) or "10" + address + 64 data bits + "0"
// (write). The address is 6 bits on the 512-byte chip and 14 bits (low 10 used) on the
// 8 KB chip, so the DMA length of the first request tells the two apart. A read returns
// 4 junk bits, then 64 data bits MSB first; at rest the chip reports ready (1).
namespace anygba::save {

namespace {

class Eeprom final : public Backup {
public:
    explicit Eeprom(std::size_t size) {
        if (size) {
            set_size(size);
        }
    }

    Type type() const override {
        return data_.empty() ? Type::Eeprom : data_.size() == 512 ? Type::Eeprom512 : Type::Eeprom8k;
    }
    std::uint8_t read8(std::uint32_t) override { return 0xFF; }
    void write8(std::uint32_t, std::uint8_t) override {}
    bool is_eeprom() const override { return true; }

    std::uint16_t eeprom_read() override {
        if (read_pos_ >= 68) {
            return 1;
        }
        const std::uint32_t pos = read_pos_++;
        if (pos < 4) {
            return 0;
        }
        const std::uint32_t bit = pos - 4;
        return static_cast<std::uint16_t>((data_[read_block_ * 8 + bit / 8] >> (7 - bit % 8)) & 1u);
    }

    void eeprom_write(std::uint16_t value, std::uint32_t dma_count) override {
        if (data_.empty()) {
            if (dma_count == 9 || dma_count == 73) {
                set_size(512);
            } else if (dma_count == 17 || dma_count == 81) {
                set_size(8192);
            } else {
                throw std::runtime_error("EEPROM accessed before its size is known (transfer of " +
                                         std::to_string(dma_count) +
                                         " halfwords); set save = \"eeprom512\" or \"eeprom8k\" in the game config");
            }
        }
        bits_.push_back(static_cast<std::uint8_t>(value & 1u));
        if (bits_.size() == 2 && bits_[0] != 1) {
            bits_.clear();  // not a request; the chip waits for a start bit
            return;
        }
        if (bits_.size() < 2) {
            return;
        }
        const bool read = bits_[1] == 1;
        const std::size_t total = 2 + width_ + (read ? 0 : 64) + 1;
        if (bits_.size() < total) {
            return;
        }
        std::uint32_t addr = 0;
        for (std::size_t i = 0; i < width_; ++i) {
            addr = (addr << 1) | bits_[2 + i];
        }
        const std::uint32_t block = addr & static_cast<std::uint32_t>(data_.size() / 8 - 1);
        if (read) {
            read_block_ = block;
            read_pos_ = 0;
        } else {
            bool any = false;
            for (std::size_t byte = 0; byte < 8; ++byte) {
                std::uint8_t v = 0;
                for (std::size_t b = 0; b < 8; ++b) {
                    v = static_cast<std::uint8_t>((v << 1) | bits_[2 + width_ + byte * 8 + b]);
                }
                std::uint8_t& cell = data_[block * 8 + byte];
                any = any || cell != v;
                cell = v;
            }
            if (any) {
                changed();
            }
            read_pos_ = 68;
        }
        bits_.clear();
    }

    void load(std::vector<std::uint8_t> bytes) override {
        if (bytes.size() != 512 && bytes.size() != 8192) {
            throw std::runtime_error("EEPROM save must be 512 or 8192 bytes, got " + std::to_string(bytes.size()));
        }
        if (!data_.empty() && bytes.size() != data_.size()) {
            throw std::runtime_error("EEPROM save holds " + std::to_string(bytes.size()) + " bytes but the chip is " +
                                     std::to_string(data_.size()) + " bytes");
        }
        set_size(bytes.size());
        data_ = std::move(bytes);
    }

private:
    void set_size(std::size_t size) {
        data_.assign(size, 0xFF);
        width_ = size == 512 ? 6 : 14;
    }

    std::size_t width_ = 0;
    std::vector<std::uint8_t> bits_;
    std::uint32_t read_block_ = 0;
    std::uint32_t read_pos_ = 68;
};

}  // namespace

std::unique_ptr<Backup> make_eeprom(std::size_t size) {
    if (size != 0 && size != 512 && size != 8192) {
        throw std::invalid_argument("EEPROM size must be 512 or 8192");
    }
    return std::make_unique<Eeprom>(size);
}

}  // namespace anygba::save
