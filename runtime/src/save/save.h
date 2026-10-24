#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Cartridge backup memory (GBATEK "GBA Cart Backup IDs" and the SRAM, Flash and EEPROM
// sections). One object per cartridge; the bus forwards 0x0E/0x0F accesses and, for EEPROM,
// halfword accesses in the 0x0D region.
namespace anygba::state {
class Archive;
}

namespace anygba::save {

enum class Type : std::uint8_t {
    None,
    Sram,       // 32 KB
    Flash64,    // 64 KB, Panasonic ID
    Flash128,   // 128 KB, Sanyo ID, two banks
    Eeprom,     // 512 B or 8 KB, decided by the first DMA transfer (or the save file size)
    Eeprom512,
    Eeprom8k,
};

// Library ID strings the Nintendo SDK links into the ROM: EEPROM_V, SRAM_V, SRAM_F_V,
// FLASH_V, FLASH512_V, FLASH1M_V. No string means no backup memory.
Type detect(std::span<const std::uint8_t> rom);

// Names used by the per-game config, --save-type, and messages: "none", "sram", "flash64",
// "flash128", "eeprom", "eeprom512", "eeprom8k". parse() returns nullopt for anything else.
std::optional<Type> parse(std::string_view name);
const char* name(Type type);

class Backup {
public:
    virtual ~Backup() = default;

    virtual Type type() const = 0;
    // 0x0E000000-0x0FFFFFFF, 8-bit bus.
    virtual std::uint8_t read8(std::uint32_t addr) = 0;
    virtual void write8(std::uint32_t addr, std::uint8_t value) = 0;

    // EEPROM serial port, one bit per halfword access. `dma_count` is the length of the DMA
    // transfer the access belongs to (0 for a CPU access), which is how the address width,
    // and so the chip size, is inferred.
    virtual bool is_eeprom() const { return false; }
    virtual std::uint16_t eeprom_read() { return 1; }
    virtual void eeprom_write(std::uint16_t /*bit*/, std::uint32_t /*dma_count*/) {}

    // Persistent contents. Empty while an EEPROM's size is still unknown.
    const std::vector<std::uint8_t>& data() const { return data_; }
    // Replaces the contents with a save file's bytes. Throws std::runtime_error if the size
    // does not fit this chip.
    virtual void load(std::vector<std::uint8_t> bytes) = 0;
    // Increments on every change of data(); persistence compares it to decide when to write.
    std::uint64_t generation() const { return generation_; }

    // Contents and chip state for save states. Loading counts as a change.
    virtual void serialize(state::Archive& ar);

protected:
    void changed() { ++generation_; }
    std::vector<std::uint8_t> data_;

private:
    std::uint64_t generation_ = 0;
};

std::unique_ptr<Backup> make_none();
std::unique_ptr<Backup> make_sram();
std::unique_ptr<Backup> make_flash(bool banks2);
std::unique_ptr<Backup> make_eeprom(std::size_t size);  // 0 = decide later, else 512 or 8192
std::unique_ptr<Backup> make(Type type);

// A .sav from another emulator (VisualBoyAdvance, mGBA: raw chip contents) checked against the
// chip `type`: exactly the chip's size, or that size plus a footer of at most kMaxFooter bytes
// (VisualBoyAdvance-M's real-time clock bytes, other emulators' trailers), which is cut off. An
// EEPROM of unknown size takes 512 or 8192 bytes. Returns the bytes to load, or an error.
inline constexpr std::size_t kMaxFooter = 0x200;
std::optional<std::vector<std::uint8_t>> fit_import(Type type, std::vector<std::uint8_t> bytes, std::string* error);

// Writes `bytes` to `path` through `<path>.tmp` and a rename, so a crash never leaves half a
// file; with `keep_backup` the current file is first copied to `<path>.bak`. Each step is
// retried for about half a second while the file is locked by another program (OneDrive or a
// virus scanner syncing it). Returns an error message.
std::optional<std::string> replace_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                        bool keep_backup);

// Save file next to the per-user data, written atomically: the new contents go to
// `<path>.tmp`, the current file is copied to `<path>.bak`, then the temp file replaces it.
// Writes happen once the contents have been stable for `debounce_frames` frames, and on
// flush().
class SaveFile {
public:
    SaveFile(std::filesystem::path path, Backup& backup, std::uint32_t debounce_frames = 30);

    const std::filesystem::path& path() const { return path_; }
    // Loads the file into the backup if it exists. Throws std::runtime_error naming the file
    // when it cannot be read or does not fit the chip.
    void load();
    // Call once per frame. Returns an error message if a due write failed (it is retried on
    // the next change and on flush()).
    std::optional<std::string> on_frame();
    // Writes pending changes now. Returns an error message on failure.
    std::optional<std::string> flush();
    // Writes the contents now, changed or not (after Backup::load replaced them).
    std::optional<std::string> write_now() { return write(); }

private:
    std::optional<std::string> write();

    std::filesystem::path path_;
    Backup& backup_;
    std::uint32_t debounce_;
    std::uint64_t written_generation_ = 0;
    std::uint64_t seen_generation_ = 0;
    std::uint32_t stable_frames_ = 0;
};

}  // namespace anygba::save
