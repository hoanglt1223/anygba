#include "bios/bios.h"

#include <cstdio>

// SWI dispatch. Every GBA BIOS function 0x00-0x2A is either implemented natively or, for
// the BIOS sound driver and multiboot, a stub that says so once on stderr. Numbers past
// 0x2A crash a real GBA; here they stop the run with an error.
namespace anygba {

namespace {

constexpr const char* kNames[0x2B] = {
    "SoftReset", "RegisterRamReset", "Halt", "Stop", "IntrWait", "VBlankIntrWait", "Div", "DivArm",
    "Sqrt", "ArcTan", "ArcTan2", "CpuSet", "CpuFastSet", "GetBiosChecksum", "BgAffineSet", "ObjAffineSet",
    "BitUnPack", "LZ77UnCompWram", "LZ77UnCompVram", "HuffUnComp", "RLUnCompWram", "RLUnCompVram",
    "Diff8bitUnFilterWram", "Diff8bitUnFilterVram", "Diff16bitUnFilter", "SoundBias", "SoundDriverInit",
    "SoundDriverMode", "SoundDriverMain", "SoundDriverVSync", "SoundChannelClear", "MidiKey2Freq",
    "SoundWhatever0", "SoundWhatever1", "SoundWhatever2", "SoundWhatever3", "SoundWhatever4", "MultiBoot",
    "HardReset", "CustomHalt", "SoundDriverVSyncOff", "SoundDriverVSyncOn", "SoundGetJumpList",
};

constexpr std::uint16_t kStopWakeIrqs = 0x3080;  // keypad, game pak, serial

}  // namespace

void System::swi(std::uint32_t number) {
    anygba_cpu& c = cpu_;
    const std::uint32_t call_pc = c.r[15] - (c.t ? 2u : 4u);
    last_bios_opcode_ = bios::kOpcodeAfterSwi;
    bios::spend(*this, 50);
    switch (number) {
    case 0x00:
        bios::soft_reset(*this);
        unwind();
    case 0x01: bios::register_ram_reset(*this, c.r[0]); break;
    case 0x02:
        halt_until_irq(0x3FFF);
        run_events();
        deliver_irq();
        break;
    case 0x03:
        halt_until_irq(kStopWakeIrqs);
        run_events();
        deliver_irq();
        break;
    case 0x04: intr_wait(c.r[0] != 0, static_cast<std::uint16_t>(c.r[1])); break;
    case 0x05: intr_wait(true, 1); break;
    case 0x06: bios::div(*this, static_cast<std::int32_t>(c.r[0]), static_cast<std::int32_t>(c.r[1])); break;
    case 0x07: bios::div(*this, static_cast<std::int32_t>(c.r[1]), static_cast<std::int32_t>(c.r[0])); break;
    case 0x08: bios::sqrt(*this); break;
    case 0x09: bios::arctan(*this); break;
    case 0x0A: bios::arctan2(*this); break;
    case 0x0B: bios::cpu_set(*this); break;
    case 0x0C: bios::cpu_fast_set(*this); break;
    case 0x0D:
        c.r[0] = 0xBAAE187F;  // checksum of the GBA BIOS
        c.r[1] = 1;
        c.r[3] = Memory::kBiosSize;
        break;
    case 0x0E: bios::bg_affine_set(*this); break;
    case 0x0F: bios::obj_affine_set(*this); break;
    case 0x10: bios::bit_unpack(*this); break;
    case 0x11: bios::lz77(*this, false); break;
    case 0x12: bios::lz77(*this, true); break;
    case 0x13: bios::huffman(*this); break;
    case 0x14: bios::run_length(*this, false); break;
    case 0x15: bios::run_length(*this, true); break;
    case 0x16: bios::diff_unfilter(*this, 1, false); break;
    case 0x17: bios::diff_unfilter(*this, 1, true); break;
    case 0x18: bios::diff_unfilter(*this, 2, true); break;
    case 0x19: {
        // The BIOS ramps the level in small steps; only the end value is observable.
        const std::uint16_t bias = io_read16(0x088);
        io_write16(0x088, static_cast<std::uint16_t>((bias & ~0x3FFu) | (c.r[0] ? 0x200u : 0u)));
        bios::spend(*this, 8 * 0x200);
        break;
    }
    case 0x1F: bios::midi_key_to_freq(*this); break;
    case 0x25:
        stub_swi(number, call_pc);
        c.r[0] = 1;  // multiboot failed: there is no link cable
        break;
    case 0x26:
        reset();
        unwind();
    case 0x27:
        if (c.r[2] & 0x80) {
            halt_until_irq(kStopWakeIrqs);
        } else {
            halt_until_irq(0x3FFF);
        }
        run_events();
        deliver_irq();
        break;
    case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
    case 0x28: case 0x29: case 0x2A:
        stub_swi(number, call_pc);
        break;
    default: {
        char msg[96];
        std::snprintf(msg, sizeof msg, "unknown BIOS call SWI 0x%02X at 0x%08X", number, call_pc);
        fail(msg);
    }
    }
    last_bios_opcode_ = bios::kOpcodeAfterSwi;
}

void System::stub_swi(std::uint32_t number, std::uint32_t call_pc) {
    const std::uint64_t bit = std::uint64_t{1} << number;
    if (!(stubs_logged_ & bit)) {
        stubs_logged_ |= bit;
        std::fprintf(stderr, "anygba: BIOS %s (SWI 0x%02X at 0x%08X) is not emulated; ignoring it\n", kNames[number],
                     number, call_pc);
    }
}

}  // namespace anygba

extern "C" void anygba_swi(anygba_cpu* c, std::uint32_t number) { anygba::system_of(c).swi(number); }
