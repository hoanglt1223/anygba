#pragma once

#include <cstdint>

// Decoded ARMv4T instruction. ARM and Thumb share one representation: every Thumb
// instruction is expressed as its ARM equivalent, with `thumb_format` kept for the
// disassembler and for the few Thumb-only behaviours (PC alignment, BL halves).
namespace anygba::decode {

enum class Cond : std::uint8_t { EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL, NV };

enum class Shift : std::uint8_t { LSL, LSR, ASR, ROR, RRX };

enum class Op : std::uint8_t {
    // Data processing, in ARM opcode order so `static_cast<Op>(opcode)` works.
    AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC, MVN,
    MUL, MLA, UMULL, UMLAL, SMULL, SMLAL,
    SWP, SWPB,
    LDR, STR, LDRB, STRB, LDRH, STRH, LDRSB, LDRSH,
    LDM, STM,
    B, BL, BX,
    BL_HI,  // lone Thumb BL prefix: LR = PC + (imm << 12)
    BL_LO,  // lone Thumb BL suffix: PC = LR + imm, LR = next | 1
    SWI, MRS, MSR,
    UNDEFINED,
};

inline constexpr std::uint8_t kSP = 13;
inline constexpr std::uint8_t kLR = 14;
inline constexpr std::uint8_t kPC = 15;

struct Insn {
    std::uint32_t addr = 0;
    std::uint32_t raw = 0;  // Thumb BL pair: first halfword in the low 16 bits
    std::uint8_t size = 4;
    bool thumb = false;
    std::uint8_t thumb_format = 0;  // 1..19 per the ARM7TDMI datasheet, 0 for ARM

    Op op = Op::UNDEFINED;
    Cond cond = Cond::AL;
    bool s = false;  // data processing / multiply: set flags. LDM/STM: the ^ bit.

    std::uint8_t rd = 0;  // long multiply: RdLo
    std::uint8_t rn = 0;  // long multiply: RdHi; MLA: accumulate register
    std::uint8_t rm = 0;
    std::uint8_t rs = 0;

    // Second operand (data processing) or offset (transfers).
    bool imm = false;
    std::uint32_t imm_value = 0;  // DP: rotated value. Transfers: offset magnitude.
                                  // B/BL: absolute target. SWI: comment. BL halves: signed offset.
    std::uint8_t imm_rot = 0;     // DP immediate rotate amount (0..30); carry-out depends on it
    Shift shift = Shift::LSL;
    bool shift_by_reg = false;
    std::uint8_t shift_amount = 0;  // immediate shifts, normalised: LSR/ASR #32 stored as 32

    // Transfers
    bool pre = true;
    bool up = true;
    bool writeback = false;
    bool user = false;  // LDRT/STRT/LDRBT/STRBT
    std::uint16_t reglist = 0;

    // PSR transfers
    bool spsr = false;
    std::uint8_t psr_mask = 0;  // MSR field mask: bit0 c, bit1 x, bit2 s, bit3 f

    // Thumb PC-relative loads and ADD Rd, PC use (PC & ~3) as the base.
    bool pc_align = false;

    bool valid() const { return op != Op::UNDEFINED; }
    std::uint32_t next() const { return addr + size; }
    // Value an instruction sees when it reads R15 as an operand.
    std::uint32_t pc_value() const { return addr + (thumb ? 4u : 8u); }
};

}  // namespace anygba::decode
