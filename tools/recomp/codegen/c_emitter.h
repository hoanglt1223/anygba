#pragma once

#include "analysis/functions.h"
#include "analysis/image.h"
#include "decode/insn.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace anygba::codegen {

std::string function_name(std::uint32_t addr);

struct EmitOptions {
    // When set, PC-relative loads from ROM are folded into constants read from here.
    const analysis::Image* image = nullptr;
    // Functions that exist in the generated program, for direct calls; BL to anything
    // else becomes an indirect call through the dispatcher.
    const std::map<std::uint32_t, analysis::Function>* functions = nullptr;
    std::string name;  // overrides function_name(entry)
};

// Emits one C11 function `void f_XXXXXXXX(anygba_cpu* c)` for `fn`.
std::string emit_function(const analysis::Function& fn, const EmitOptions& options);

// The bytes `fn`'s compiled body depends on: its instructions, the literal-pool words
// emit_function folds into constants (when `image` is given), and its jump tables, as an
// address range in the function's own address space (ROM or RAM).
analysis::AddressRange code_footprint(const analysis::Function& fn, const analysis::Image* image);

// A function routed to a C++ override (docs/dev/OVERRIDES.md). Its translation stays in the
// program as f_XXXXXXXX_orig; f_XXXXXXXX runs the override while the runtime has it switched on.
struct OverrideInfo {
    std::uint32_t addr = 0;
    bool thumb = true;
    std::string impl;    // name registered with ANYGBA_OVERRIDE
    std::string name;    // symbol, or the address in hex
    std::string engine;  // "" when untagged
    bool enabled = true;
};

struct ProjectInfo {
    std::string title;
    std::string rom_sha256;
    std::uint32_t rom_size = 0;
    std::size_t shards = 0;  // number of funcs_NNN.c files; 0 = one per 500 functions
    std::string save_type;   // "" = detect at run time
    int rtc = -1;            // 1 on, 0 off, -1 decided at run time
    std::string video;       // serialized video settings, "" = defaults
    std::map<std::uint32_t, std::string> replacements;  // function entry -> hand-written C function
    std::vector<std::filesystem::path> sources;        // copied next to the generated files
    std::vector<OverrideInfo> overrides;               // sorted by addr
};

// Writes the generated program: game.h, funcs_NNN.c, dispatch.c and CMakeLists.txt.
// Returns the files, in that order.
std::vector<std::filesystem::path> write_project(const std::filesystem::path& dir, const analysis::Image& image,
                                                 const analysis::Discovery& discovery, const ProjectInfo& info);

}  // namespace anygba::codegen
