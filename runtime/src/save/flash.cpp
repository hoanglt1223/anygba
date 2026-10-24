#include "save/save.h"
#include "state/archive.h"

#include <algorithm>
#include <stdexcept>

// Flash backup (GBATEK "GBA Cart Backup Flash ROM"). Commands are byte writes to 0x5555 and
// 0x2AAA in the sequence AA, 55, cmd. Erase and program complete instantly, so status polls
// see the final value straight away.
namespace anygba::save {

namespace {

constexpr std::uint32_t kBank = 0x10000;
constexpr std::uint32_t kSector = 0x1000;

class Flash final : public Backup {
public:
    explicit Flash(bool banks2) : banks2_(banks2) { data_.assign(banks2 ? 2 * kBank : kBank, 0xFF); }

    Type type() const override { return banks2_ ? Type::Flash128 : Type::Flash64; }

    std::uint8_t read8(std::uint32_t addr) override {
        const std::uint32_t off = addr & (kBank - 1);
        if (id_mode_ && off < 2) {
            // Sanyo LE26FV10N1TS (128 KB) and Panasonic MN63F805MNP (64 KB), as mGBA reports.
            static constexpr std::uint8_t kId[2][2] = {{0x32, 0x1B}, {0x62, 0x13}};
            return kId[banks2_ ? 1 : 0][off];
        }
        return data_[bank_ * kBank + off];
    }

    void write8(std::uint32_t addr, std::uint8_t value) override {
        const std::uint32_t off = addr & (kBank - 1);
        if (mode_ == Mode::Program) {
            set(bank_ * kBank + off, value);
            mode_ = Mode::Ready;
            return;
        }
        if (mode_ == Mode::Bank) {
            if (off == 0) {
                bank_ = banks2_ ? (value & 1u) : 0;
            }
            mode_ = Mode::Ready;
            return;
        }
        switch (step_) {
        case 0:
            step_ = (off == 0x5555 && value == 0xAA) ? 1 : 0;
            return;
        case 1:
            step_ = (off == 0x2AAA && value == 0x55) ? 2 : 0;
            return;
        default:
            break;
        }
        step_ = 0;
        if (erase_armed_) {
            erase_armed_ = false;
            if (off == 0x5555 && value == 0x10) {
                std::fill(data_.begin(), data_.end(), 0xFF);
                changed();
            } else if (value == 0x30) {
                const auto begin = data_.begin() + bank_ * kBank + (off & ~(kSector - 1));
                std::fill(begin, begin + kSector, 0xFF);
                changed();
            }
            return;
        }
        if (off != 0x5555) {
            return;
        }
        switch (value) {
        case 0x90: id_mode_ = true; break;
        case 0xF0: id_mode_ = false; break;
        case 0x80: erase_armed_ = true; break;
        case 0xA0: mode_ = Mode::Program; break;
        case 0xB0: mode_ = banks2_ ? Mode::Bank : Mode::Ready; break;
        default: break;
        }
    }

    void load(std::vector<std::uint8_t> bytes) override {
        if (bytes.size() != data_.size()) {
            throw std::runtime_error(std::string(name(type())) + " save must be " + std::to_string(data_.size()) +
                                     " bytes, got " + std::to_string(bytes.size()));
        }
        data_ = std::move(bytes);
    }

    void serialize(state::Archive& ar) override {
        Backup::serialize(ar);
        ar.pod(bank_);
        ar.pod(step_);
        ar.pod(id_mode_);
        ar.pod(erase_armed_);
        ar.pod(mode_);
        if (ar.loading() && (bank_ > (banks2_ ? 1u : 0u) || mode_ > Mode::Bank)) {
            throw Error("state file has an invalid Flash chip state");
        }
    }

private:
    enum class Mode : std::uint8_t { Ready, Program, Bank };

    void set(std::uint32_t index, std::uint8_t value) {
        if (data_[index] != value) {
            data_[index] = value;
            changed();
        }
    }

    bool banks2_;
    std::uint32_t bank_ = 0;
    int step_ = 0;
    bool id_mode_ = false;
    bool erase_armed_ = false;
    Mode mode_ = Mode::Ready;
};

}  // namespace

std::unique_ptr<Backup> make_flash(bool banks2) { return std::make_unique<Flash>(banks2); }

}  // namespace anygba::save
