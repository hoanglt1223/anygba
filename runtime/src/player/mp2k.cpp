#include "player/mp2k.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace anygba::player {

namespace {

constexpr std::uint32_t kSoundInfoPtr = 0x03007FF0;
constexpr std::uint32_t kLockMax = 8;
// SoundInfo and MusicPlayerInfo field offsets (pret/pokeemerald include/gba/m4a_internal.h).
constexpr std::uint32_t kInfoPlayerHead = 0x24;
constexpr std::uint32_t kPlayerTempoD = 0x1C;
constexpr std::uint32_t kPlayerTempoU = 0x1E;
constexpr std::uint32_t kPlayerTempoI = 0x20;
constexpr std::uint32_t kPlayerIdent = 0x34;
constexpr std::uint32_t kPlayerNext = 0x3C;
constexpr int kMaxPlayers = 32;
// SoundInfo.chans: DirectSound channels (struct SoundChannel), and their envelope fields.
constexpr std::uint32_t kInfoChannels = 0x50;
constexpr std::size_t kStatus = 0x00;
constexpr std::size_t kAttack = 0x04;
constexpr std::size_t kDecay = 0x05;
constexpr std::size_t kRelease = 0x07;

// Work RAM bytes at `addr`, or null outside EWRAM/IWRAM (mirrors included).
std::uint8_t* ram(Memory& m, std::uint32_t addr, std::uint32_t size) {
    switch (addr >> 24) {
    case 0x02: {
        const std::uint32_t off = addr & (Memory::kEwramSize - 1);
        return off + size <= Memory::kEwramSize ? m.ewram.data() + off : nullptr;
    }
    case 0x03: {
        const std::uint32_t off = addr & (Memory::kIwramSize - 1);
        return off + size <= Memory::kIwramSize ? m.iwram.data() + off : nullptr;
    }
    default: return nullptr;
    }
}

bool read32(Memory& m, std::uint32_t addr, std::uint32_t& value) {
    const std::uint8_t* p = ram(m, addr, 4);
    if (!p) return false;
    value = static_cast<std::uint32_t>(p[0] | p[1] << 8 | p[2] << 16) | static_cast<std::uint32_t>(p[3]) << 24;
    return true;
}

std::uint16_t read16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

void write16(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

}  // namespace

// The mixer steps each DirectSound channel's envelope once per frame: attack adds `attack` up
// to 255, decay and release multiply by `decay` / `release` / 256. A note copies the values
// from its instrument when it starts, so a channel whose values differ from the ones last
// written here holds a new note; its values are slowed by the speed (attack divided, the
// multipliers raised to 1/speed) and the originals kept to put back at 1x.
void Mp2kTempo::scale_envelopes(std::uint8_t* chans, int percent) {
    for (int c = 0; c < kChannels; ++c) {
        std::uint8_t* ch = chans + c * kChannelSize;
        Envelope& e = envelopes_[static_cast<std::size_t>(c)];
        const std::array<std::uint8_t, 3> now = {ch[kAttack], ch[kDecay], ch[kRelease]};
        if (percent <= 100) {
            if (e.scaled && now == e.written) {
                ch[kAttack] = e.original[0];
                ch[kDecay] = e.original[1];
                ch[kRelease] = e.original[2];
            }
            e.scaled = false;
            continue;
        }
        if (ch[kStatus] == 0) continue;  // idle channel
        if (e.scaled && now == e.written) continue;
        const double n = percent / 100.0;
        e.original = now;
        e.written[0] = static_cast<std::uint8_t>(std::max(1L, std::lround(now[0] / n)));
        for (int i = 1; i < 3; ++i) {
            e.written[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(
                std::min(255L, std::lround(256.0 * std::pow(now[static_cast<std::size_t>(i)] / 256.0, 1.0 / n))));
        }
        ch[kAttack] = e.written[0];
        ch[kDecay] = e.written[1];
        ch[kRelease] = e.written[2];
        e.scaled = true;
    }
}

bool Mp2kTempo::update(Memory& memory, int percent) {
    std::uint32_t info = 0;
    std::uint32_t ident = 0;
    found_ = read32(memory, kSoundInfoPtr, info) && read32(memory, info, ident) && ident - kIdent <= kLockMax;
    // Locked: the engine is in the middle of its work; try again next frame.
    if (!found_ || ident != kIdent) return found_;
    const bool scale = percent > 100;

    std::uint32_t player = 0;
    if (!read32(memory, info + kInfoPlayerHead, player)) return true;
    std::set<std::uint32_t> seen;
    for (int i = 0; i < kMaxPlayers && player && seen.insert(player).second; ++i) {
        std::uint8_t* p = ram(memory, player, 0x40);
        if (!p) break;
        std::uint32_t player_ident = 0;
        read32(memory, player + kPlayerIdent, player_ident);
        if (player_ident == kIdent) {
            const std::uint32_t tempo = (static_cast<std::uint32_t>(read16(p + kPlayerTempoD)) * read16(p + kPlayerTempoU)) >> 8;
            if (scale) {
                // Carry the fraction so the long-run tempo is exact.
                double& carry = carry_[player];
                const double exact = tempo * 100.0 / percent + carry;
                const double whole = std::floor(exact);
                carry = exact - whole;
                write16(p + kPlayerTempoI, static_cast<std::uint32_t>(whole));
            } else {
                write16(p + kPlayerTempoI, tempo);
            }
        }
        if (!read32(memory, player + kPlayerNext, player)) break;
    }
    if (std::uint8_t* chans = ram(memory, info + kInfoChannels, kChannels * kChannelSize)) {
        scale_envelopes(chans, scale ? percent : 100);
    }
    scaled_ = scale;
    if (!scale) carry_.clear();
    return true;
}

}  // namespace anygba::player
