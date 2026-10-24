#pragma once

#include "anygba/system.hpp"

#include <array>
#include <cstdint>
#include <map>

// Normal-speed music for games using Nintendo's MusicPlayer2000 (m4a / "Sappy") sound engine:
// while the game runs faster, every music player's tempo is divided by the speed, so songs and
// sound effects keep their 1x tempo in wall-clock time.
//
// The engine keeps its SoundInfo at the address stored in 0x03007FF0 (SOUND_INFO_PTR), marked
// with ident 'Smsh' (0x68736D53; ident + 1..8 while it is locked). mGBA finds the same struct
// from the sound DMA source (SoundInfo + 0x350 is the PCM buffer). From there the music players
// form a list (SoundInfo.musicPlayerHead, MusicPlayerInfo.musicPlayerNext), each with
// ident 'Smsh'. A player advances one sequencer tick per 150 of tempoC, adding tempoI every
// frame; the engine keeps tempoI = tempoD * tempoU >> 8 (pret/pokeemerald m4a.c).
namespace anygba::player {

class Mp2kTempo {
public:
    static constexpr std::uint32_t kIdent = 0x68736D53;

    // Call once per frame at VBlank. `percent` is the game speed; 100 puts the engine's own
    // tempo back. Returns whether an MP2K engine was found.
    bool update(Memory& memory, int percent);
    bool found() const { return found_; }
    // Tempos are divided now; call update(memory, 100) once to put them back.
    bool scaled() const { return scaled_; }

    static constexpr int kChannels = 12;
    static constexpr std::uint32_t kChannelSize = 0x40;

private:
    struct Envelope {
        bool scaled = false;
        std::array<std::uint8_t, 3> original{};  // attack, decay, release of the note
        std::array<std::uint8_t, 3> written{};
    };
    void scale_envelopes(std::uint8_t* chans, int percent);

    std::array<Envelope, kChannels> envelopes_{};
    bool found_ = false;
    bool scaled_ = false;
    std::map<std::uint32_t, double> carry_;  // per player: fraction of tempoI not yet applied
};

}  // namespace anygba::player
