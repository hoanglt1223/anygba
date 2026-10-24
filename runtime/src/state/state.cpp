#include "anygba/crc32.h"
#include "anygba/sha256.h"
#include "anygba/system.hpp"
#include "apu/apu.h"
#include "ppu/ppu.h"
#include "rtc.h"
#include "save/save.h"
#include "state/archive.h"

#include <array>
#include <bit>
#include <cstring>
#include <string>

// Save-state file format and the per-component serialization (docs/dev/SAVE-STATES.md).
namespace anygba {

static_assert(std::endian::native == std::endian::little, "state files are little-endian");

namespace {

constexpr std::array<char, 8> kMagic = {'A', 'G', 'B', 'S', 'T', 'A', 'T', 'E'};

// Fixed-size header in front of the payload.
struct Header {
    std::array<char, 8> magic = kMagic;
    std::uint32_t version = System::kStateVersion;
    std::uint32_t flags = 0;
    std::array<char, 64> rom_sha256{};
    std::uint64_t frames = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t payload_crc32 = 0;
};
static_assert(sizeof(Header) == 96);

}  // namespace

const std::string& System::rom_sha256() {
    if (rom_sha256_.empty()) {
        rom_sha256_ = sha256_hex({mem_.rom.data(), mem_.rom_size});
    }
    return rom_sha256_;
}

void System::serialize(state::Archive& ar) {
    anygba_cpu& c = cpu_;
    ar.pod(c.r);
    ar.pod(c.n);
    ar.pod(c.z);
    ar.pod(c.c);
    ar.pod(c.v);
    ar.pod(c.t);
    ar.pod(c.ctrl);
    ar.pod(c.spsr);
    ar.pod(c.cycles);
    ar.pod(c.bank_r8_12);
    ar.pod(c.bank_r13_14);
    ar.pod(c.bank_spsr);

    ar.fixed(mem_.bios, "BIOS area");
    ar.fixed(mem_.ewram, "EWRAM");
    ar.fixed(mem_.iwram, "IWRAM");
    ar.fixed(mem_.io, "IO registers");
    ar.fixed(mem_.palette, "palette");
    ar.fixed(mem_.vram, "VRAM");
    ++vram_writes_;
    ar.fixed(mem_.oam, "OAM");

    ar.pod(budget_start_);
    ar.pod(budget_);
    ar.pod(events_);
    ar.fields(line_, hblank_, frames_, halted_, in_irq_);
    for (DmaChannel& d : dma_) {
        ar.fields(d.src, d.dst, d.count, d.control, d.active);
    }
    for (Timer& t : timers_) {
        ar.fields(t.reload, t.control, t.counter, t.started);
    }
    ar.pod(framebuffer_);
    ar.pod(eeprom_dma_count_);
    ar.pod(last_bios_opcode_);
    if (ar.loading() && line_ >= 228) {
        throw Error("state file has an invalid scanline");
    }

    ppu_->serialize(ar);
    apu_->serialize(ar);
    backup_->serialize(ar);
    // A cartridge clock changes the layout; the ROM hash already ties a state to its cartridge.
    if (rtc_) {
        rtc_->serialize(ar);
    }
}

std::vector<std::uint8_t> System::save_state() {
    state::Archive payload_ar = state::Archive::writer();
    serialize(payload_ar);
    const std::vector<std::uint8_t> payload = payload_ar.take();

    Header h;
    std::memcpy(h.rom_sha256.data(), rom_sha256().data(), h.rom_sha256.size());
    h.frames = frames_;
    h.payload_size = static_cast<std::uint32_t>(payload.size());
    h.payload_crc32 = crc32(payload);

    std::vector<std::uint8_t> image(sizeof h + payload.size());
    std::memcpy(image.data(), &h, sizeof h);
    std::memcpy(image.data() + sizeof h, payload.data(), payload.size());
    return image;
}

void System::load_state(std::span<const std::uint8_t> image) {
    Header h;
    if (image.size() < sizeof h) {
        throw Error("not a save state (file too short)");
    }
    std::memcpy(&h, image.data(), sizeof h);
    if (h.magic != kMagic) {
        throw Error("not a save state");
    }
    if (h.version != kStateVersion) {
        throw Error("save state format version " + std::to_string(h.version) + " is not supported (this program reads " +
                    std::to_string(kStateVersion) + ")");
    }
    if (std::string(h.rom_sha256.data(), h.rom_sha256.size()) != rom_sha256()) {
        throw Error("save state belongs to another ROM (SHA-256 " + std::string(h.rom_sha256.data(), 8) + "...)");
    }
    const std::span<const std::uint8_t> payload = image.subspan(sizeof h);
    if (payload.size() != h.payload_size || crc32(payload) != h.payload_crc32) {
        throw Error("save state is damaged (size or checksum mismatch)");
    }

    // A file that passes the checksum can still disagree with this build's layout; keep the
    // current state so a failed load changes nothing.
    state::Archive rollback_ar = state::Archive::writer();
    serialize(rollback_ar);
    const std::vector<std::uint8_t> rollback = rollback_ar.take();
    try {
        state::Archive ar = state::Archive::reader(payload);
        serialize(ar);
        if (!ar.at_end()) {
            throw Error("save state has unexpected trailing data");
        }
    } catch (...) {
        state::Archive ar = state::Archive::reader(rollback);
        serialize(ar);
        throw;
    }
    cpu_.ret_depth = 0;
    resume_depth_ = 0;
    map_pages();
    update_wait_states();
    show_gpio();
}

void System::request_safe_point(std::function<void(System&)> action) { safe_point_ = std::move(action); }

void Ppu::serialize(state::Archive& ar) {
    ar.pod(ref_x_);
    ar.pod(ref_y_);
}

void Apu::serialize(state::Archive& ar) {
    const auto envelope = [&ar](Envelope& e) { ar.fields(e.volume, e.period, e.up, e.timer); };
    for (Square& s : square_) {
        ar.fields(s.on, s.duty, s.step, s.countdown, s.freq, s.length, s.length_on, s.sweep_period, s.sweep_shift,
                  s.sweep_timer, s.shadow, s.sweep_down, s.sweep_on);
        envelope(s.env);
    }
    ar.fields(wave_.on, wave_.freq, wave_.position, wave_.countdown, wave_.length, wave_.length_on);
    ar.pod(wave_.ram);
    ar.fields(noise_.on, noise_.lfsr, noise_.countdown, noise_.length, noise_.length_on, noise_.high);
    envelope(noise_.env);
    for (Fifo& f : fifo_) {
        ar.pod(f.data);
        ar.fields(f.read, f.size, f.current, f.fed);
    }
    ar.fields(sequencer_step_, samples_to_sequencer_, next_sample_, underruns_);
    if (!ar.loading()) {
        return;
    }
    out_.clear();
    bool ok = sequencer_step_ < 8 && wave_.position < 64;
    for (const Square& s : square_) {
        ok = ok && s.duty < 4 && s.step < 8;
    }
    for (const Fifo& f : fifo_) {
        ok = ok && f.read < f.data.size() && f.size <= f.data.size();
    }
    if (!ok) {
        throw Error("save state has an invalid sound state");
    }
}

}  // namespace anygba
