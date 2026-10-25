#include "anygba/system.hpp"

#include "interp/interp.h"

#include <algorithm>
#include <csetjmp>
#include <cstring>

// Address-to-function dispatch, the resume trampoline for call sites whose callee came back
// somewhere unexpected, and the run loop that owns the setjmp target for unwinds.
namespace anygba {

namespace {

constexpr int kJumpUnwind = 1;
constexpr int kJumpStop = 2;
constexpr int kJumpFail = 3;

}  // namespace

anygba_fn System::lookup(std::uint32_t addr, bool thumb) const {
    anygba_fn fn = lookup_compiled(addr, thumb);
    if (fn && (addr < 0x08000000 || addr >= 0x0E000000) && !ram_code_valid(addr, thumb)) {
        return nullptr;  // the game has replaced the code: interpret what is there now
    }
    if (fn && cpu_.rom_patched && addr >= 0x08000000 && addr < 0x0E000000 &&
        bypassed_.contains(0x08000000u + (addr & (Memory::kRomMax - 1)))) {
        return nullptr;  // a ROM patch covers its code: interpret it
    }
    return fn;
}

anygba_fn System::lookup_compiled(std::uint32_t addr, bool thumb) const {
    if (!game_) {
        return nullptr;
    }
    if (addr >= 0x08000000 && addr < 0x0E000000) {
        const std::uint32_t off = addr & (Memory::kRomMax - 1);
        if (off < mem_.rom_size && (off & 1) == 0) {
            // Generated tables hold canonical 0x08xxxxxx addresses; mirrors map onto them.
            const anygba_function_entry* e = rom_direct_[off / 2];
            if (e && (e->thumb != 0) == thumb) {
                return e->fn;
            }
        }
        return nullptr;
    }
    const anygba_function_entry* e = ram_function(addr, thumb);
    return e ? e->fn : nullptr;
}

const anygba_function_entry* System::ram_function(std::uint32_t addr, bool thumb) const {
    const auto it = std::lower_bound(ram_functions_.begin(), ram_functions_.end(), addr,
                                     [](const anygba_function_entry& f, std::uint32_t a) { return f.addr < a; });
    if (it != ram_functions_.end() && it->addr == addr && (it->thumb != 0) == thumb) {
        return &*it;
    }
    return nullptr;
}

// Games reuse RAM for different code (overlays, routines built on the stack), so compiled RAM
// code is checked against the ROM bytes it came from on every entry.
bool System::ram_code_valid(std::uint32_t entry, bool thumb) const {
    const anygba_function_entry* e = ram_function(entry, thumb);
    if (!e || e->code_src < 0x08000000 || e->code_end <= e->code_begin) {
        return false;
    }
    const std::uint32_t len = e->code_end - e->code_begin;
    const std::uint32_t region = e->code_begin >> 24;
    const std::vector<std::uint8_t>* ram = region == 0x02 ? &mem_.ewram : region == 0x03 ? &mem_.iwram : nullptr;
    const std::uint32_t src = e->code_src - 0x08000000;
    if (!ram || (e->code_begin & 0x00FFFFFF) + std::uint64_t{len} > ram->size() ||
        std::uint64_t{src} + len > mem_.rom_size) {
        return false;
    }
    // Compare with the unpatched ROM: a ROM patch cheat over the source changes the copy too.
    if (cpu_.rom_patched) {
        const auto it = rom_patches_.lower_bound((0x08000000 + src) & ~1u);
        if (it != rom_patches_.end() && it->first < 0x08000000 + src + len) {
            return false;
        }
    }
    return std::memcmp(ram->data() + (e->code_begin & 0x00FFFFFF), mem_.rom.data() + src, len) == 0;
}

void System::run_once() {
    if (anygba_fn fn = lookup(cpu_.r[15], cpu_.t != 0)) {
        resuming_ = false;
        fn(&cpu_);
    } else {
        interpret();
    }
}

int System::resume(std::uint32_t expected) {
    // A callee that never comes back to its call site (a scheduler loop entered by BL, a
    // jump back into the caller's loop) nests one resume per round on the host stack,
    // while ret_depth stays flat. All guest state is in cpu_, so past a bound drop the
    // host frames and continue from r[15] in the run loop. Not inside an IRQ handler: the
    // BIOS epilogue in deliver_irq() still has to run when it returns.
    if (++resume_depth_ > kMaxResumeDepth && !in_irq_handler()) {
        unwind();
    }
    for (;;) {
        const std::uint32_t pc = cpu_.r[15];
        if (pc == expected) {
            --resume_depth_;
            return 0;
        }
        for (std::uint32_t i = cpu_.ret_depth; i-- > 0;) {
            if (cpu_.ret_stack[i] == pc) {
                --resume_depth_;
                return 1;
            }
        }
        run_once();
    }
}

bool System::in_irq_handler() const {
    for (std::uint32_t i = 0; i < cpu_.ret_depth; ++i) {
        if (cpu_.ret_stack[i] == kBiosIrqReturn) {
            return true;
        }
    }
    return false;
}

// Owns the setjmp target. Kept free of objects with destructors: longjmp skips them.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4611)  // MSVC flags every setjmp in C++; this frame holds no such objects
#endif
int System::run_guarded() {
    std::jmp_buf jb;
    top_jmp_ = &jb;
    const int why = setjmp(jb);
    if (why == kJumpStop || why == kJumpFail) {
        return why;
    }
    cpu_.ret_depth = 0;
    resume_depth_ = 0;
    swi_pc_ = kNoSwi;
    for (;;) {
        run_once();
    }
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

void System::run() {
    void* const previous = top_jmp_;
    stop_requested_ = false;
    const int why = run_guarded();
    top_jmp_ = previous;
    cpu_.ret_depth = 0;
    if (why == kJumpFail) {
        throw Error(error_);
    }
}

void System::unwind() {
    if (!top_jmp_) {
        throw Error("anygba_unwind called outside System::run");
    }
    cpu_.ret_depth = 0;
    std::longjmp(*static_cast<std::jmp_buf*>(top_jmp_), kJumpUnwind);
}

void System::fail(std::string message) {
    if (!top_jmp_) {
        throw Error(message);
    }
    error_ = std::move(message);
    std::longjmp(*static_cast<std::jmp_buf*>(top_jmp_), kJumpFail);
}

void System::log_miss(std::uint32_t addr, bool thumb) {
    if (!misses_.insert(addr).second || !options_.log_misses) {
        return;
    }
    if (!miss_file_) {
        miss_file_ = std::fopen(options_.miss_log.string().c_str(), "a");
        if (!miss_file_) {
            options_.log_misses = false;
            return;
        }
    }
    // For RAM code, name the ROM bytes it was copied from so the next pass can analyse it.
    char rom_part[24] = "";
    const bool ewram = addr >= 0x02000000 && addr < 0x03000000;
    const bool iwram = addr >= 0x03000000 && addr < 0x04000000;
    if (ewram || iwram) {
        const std::vector<std::uint8_t>& ram = ewram ? mem_.ewram : mem_.iwram;
        const std::uint32_t off = addr & static_cast<std::uint32_t>(ram.size() - 1);
        const std::size_t len = std::min<std::size_t>(32, ram.size() - off);
        const auto begin = ram.begin() + off;
        const auto rom_end = mem_.rom.begin() + mem_.rom_size;
        const auto hit = std::search(mem_.rom.begin(), rom_end, begin, begin + static_cast<std::ptrdiff_t>(len));
        if (hit != rom_end) {
            std::snprintf(rom_part, sizeof rom_part, " rom=0x%08x",
                          0x08000000u + static_cast<std::uint32_t>(hit - mem_.rom.begin()));
        }
    }
    std::fprintf(miss_file_, "0x%08x %s%s\n", addr, thumb ? "thumb" : "arm", rom_part);
    std::fflush(miss_file_);
}

void System::interpret() {
    if (!resuming_ && !lookup_compiled(cpu_.r[15], cpu_.t != 0)) {
        log_miss(cpu_.r[15], cpu_.t != 0);
    }
    resuming_ = false;
    std::uint32_t expected_next = cpu_.r[15];
    for (;;) {
        const std::uint32_t pc = cpu_.r[15];
        if (pc != expected_next) {
            // Control flow changed: stop at recompiled code or at a pending call site's return.
            if (lookup(pc, cpu_.t != 0)) {
                return;
            }
            for (std::uint32_t i = cpu_.ret_depth; i-- > 0;) {
                if (cpu_.ret_stack[i] == pc) {
                    return;
                }
            }
        }
        if (!breakpoints_.empty()) hit_breakpoint(pc);
        const decode::Insn in = interp::fetch(*this, pc, cpu_.t != 0);
        // Counted per instruction: interpret() can be left by longjmp, so a total taken on
        // return would miss those runs. A state load can move time back; skip that step.
        const std::uint64_t started = now();
        // Counted per instruction: interpret() can be left by longjmp, so a total taken on
        // return would miss those runs. A load or reset can move time back; skip that step.
        const std::uint64_t started = now();
        interp::execute(*this, in);
        if (const std::uint64_t ended = now(); ended > started) interpreted_cycles_ += ended - started;
        if (const std::uint64_t ended = now(); ended > started) interpreted_cycles_ += ended - started;
        expected_next = in.next();
        // A jump that leaves LR pointing just past it is a call. Calls made from interpreted
        // code name function entries the recompiler missed as well; log them so one feedback
        // pass also covers code that only ever ran in here.
        const bool call = cpu_.r[15] != expected_next && (cpu_.r[14] & ~1u) == expected_next;
        if (call && !lookup_compiled(cpu_.r[15], cpu_.t != 0)) {
            log_miss(cpu_.r[15], cpu_.t != 0);
        }
    }
}

}  // namespace anygba

extern "C" {

int anygba_resume(anygba_cpu* c, std::uint32_t expected) { return anygba::system_of(c).resume(expected); }

void anygba_unwind(anygba_cpu* c) { anygba::system_of(c).unwind(); }

void anygba_undefined(anygba_cpu* c, std::uint32_t addr, std::uint32_t raw) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "undefined instruction 0x%08X at 0x%08X", raw, addr);
    anygba::system_of(c).fail(msg);
}

}
