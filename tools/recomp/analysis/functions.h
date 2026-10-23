#pragma once

#include "analysis/image.h"
#include "analysis/jumptables.h"
#include "decode/insn.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace anygba::analysis {

// Why an address is believed to start a function. Weak sources are checked before use.
enum class EntrySource : std::uint8_t {
    RomEntry,
    Config,
    MissLog,
    Call,         // BL target
    Branch,       // BX/MOV PC/LDR PC to a known constant, or a tail branch
    IrqHandler,   // value stored to the BIOS IRQ vector at 0x03007FFC
    JumpTable,    // a table entry outside the jumping function
    LiteralPool,  // weak: a code-looking pointer in a literal pool
};

const char* to_string(EntrySource source);

struct EntryPoint {
    std::uint32_t addr = 0;
    Mode mode = Mode::Arm;
    EntrySource source = EntrySource::Config;
};

struct Function {
    std::uint32_t entry = 0;
    Mode mode = Mode::Arm;
    EntrySource source = EntrySource::Config;
    std::map<std::uint32_t, decode::Insn> insns;  // every reachable instruction, by address
    std::set<std::uint32_t> labels;               // entry plus every in-function branch target
    std::vector<JumpTable> jump_tables;
    std::set<std::uint32_t> tail_calls;  // functions this one branches to without returning first
    bool has_exit = false;       // contains a return or an unresolved indirect jump
    bool noreturn = false;       // no path leaves the function (after the no-return fixpoint)
    bool hit_undefined = false;  // a path ran into an undefined encoding or unreadable memory
};

struct AddressRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;  // exclusive
    bool contains(std::uint32_t addr) const { return addr >= begin && addr < end; }
};

struct DiscoveryOptions {
    std::vector<EntryPoint> entries;
    std::vector<AddressRange> skip;  // known data: never decoded
    bool scan_literal_pools = true;
};

struct Discovery {
    std::map<std::uint32_t, Function> functions;
    std::vector<std::string> notes;  // conflicts and rejected candidates, for the user
};

Discovery discover(const Image& image, const DiscoveryOptions& options);

}  // namespace anygba::analysis
