#include "analysis/functions.h"

#include "analysis/cfg.h"

#include <cstdio>
#include <deque>

namespace anygba::analysis {

const char* to_string(EntrySource source) {
    switch (source) {
    case EntrySource::RomEntry: return "rom-entry";
    case EntrySource::Config: return "config";
    case EntrySource::MissLog: return "miss-log";
    case EntrySource::Call: return "call";
    case EntrySource::Branch: return "branch";
    case EntrySource::IrqHandler: return "irq-handler";
    case EntrySource::JumpTable: return "jump-table";
    case EntrySource::LiteralPool: return "literal-pool";
    }
    return "?";
}

namespace {

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

bool is_return(const decode::Insn& in) {
    using decode::Op;
    switch (in.op) {
    case Op::BX:
        return in.rm == decode::kLR;
    case Op::LDM:
        return ((in.reglist >> decode::kPC) & 1u) != 0;
    case Op::MOV:
        return in.rd == decode::kPC && !in.imm && in.rm == decode::kLR;
    default:
        return false;
    }
}

// Literal-pool values point at data far more often than at code, and Thumb decodes almost
// any halfword. A candidate must decode cleanly, return somewhere, and start the way
// compiled code does: unconditionally in ARM, and with PUSH {.., lr} unless it is a short
// Thumb leaf.
bool plausible_function(const Function& fn, const EntryPoint& c) {
    const auto first = fn.insns.find(c.addr);
    if (fn.hit_undefined || first == fn.insns.end()) {
        return false;
    }
    if (c.mode == Mode::Arm) {
        return first->second.cond == decode::Cond::AL;
    }
    bool returns = false;
    for (const auto& [addr, in] : fn.insns) {
        returns = returns || is_return(in);
    }
    if (!returns) {
        return false;
    }
    const auto& in = first->second;
    const bool push_lr = in.op == decode::Op::STM && in.rn == decode::kSP && ((in.reglist >> decode::kLR) & 1u);
    return push_lr || fn.insns.size() <= 8;
}

class Discoverer {
public:
    Discoverer(const Image& image, const DiscoveryOptions& options) : image_(image), options_(options) {}

    Discovery run() {
        for (const auto& e : options_.entries) {
            enqueue(e);
        }
        drain();
        if (options_.scan_literal_pools) {
            try_candidates();
        }
        // Re-walk with the final entry set so branches into other functions become tail calls.
        for (auto& [addr, fn] : out_.functions) {
            WalkResult walk = walk_function(image_, {fn.entry, fn.mode, fn.source}, known_, noreturn_, options_.skip);
            fn = std::move(walk.function);
        }
        settle_noreturn();
        return std::move(out_);
    }

private:
    void enqueue(const EntryPoint& e) {
        if (const auto it = modes_.find(e.addr); it != modes_.end()) {
            if (it->second != e.mode) {
                out_.notes.push_back("entry " + hex(e.addr) + " seen as both ARM and Thumb; keeping the first (" +
                                     to_string(e.source) + " disagrees)");
            }
            return;
        }
        modes_.emplace(e.addr, e.mode);
        known_.insert(e.addr);
        queue_.push_back(e);
    }

    void drain() {
        while (!queue_.empty()) {
            const EntryPoint e = queue_.front();
            queue_.pop_front();
            WalkResult walk = walk_function(image_, e, known_, noreturn_, options_.skip);
            for (const auto& n : walk.notes) {
                out_.notes.push_back(hex(e.addr) + ": " + n);
            }
            for (const auto& found : walk.found) {
                enqueue(found);
            }
            for (const auto& c : walk.candidates) {
                candidates_.push_back(c);
            }
            out_.functions.emplace(e.addr, std::move(walk.function));
        }
    }

    // A literal-pool value is only adopted if everything reachable from it decodes cleanly,
    // which rejects nearly all pointers to data.
    void try_candidates() {
        while (!candidates_.empty()) {
            std::vector<EntryPoint> pending;
            pending.swap(candidates_);
            for (const auto& c : pending) {
                if (known_.contains(c.addr) || inside_known_code(c.addr) || skipped(c.addr)) {
                    continue;
                }
                const WalkResult walk = walk_function(image_, c, known_, noreturn_, options_.skip);
                const auto first = walk.function.insns.find(c.addr);
                if (!plausible_function(walk.function, c)) {
                    continue;
                }
                enqueue(c);
            }
            drain();
        }
    }

    // A function returns if it has an exit or tail-calls one that returns. Paths after calls
    // to functions that never return are dead (often a literal pool follows), so re-walk
    // with the growing no-return set until it stops changing.
    void settle_noreturn() {
        for (;;) {
            std::set<std::uint32_t> returns;
            for (bool changed = true; changed;) {
                changed = false;
                for (const auto& [addr, fn] : out_.functions) {
                    if (returns.contains(addr)) {
                        continue;
                    }
                    bool r = fn.has_exit;
                    for (const std::uint32_t t : fn.tail_calls) {
                        r = r || returns.contains(t) || !out_.functions.contains(t);
                    }
                    if (r) {
                        returns.insert(addr);
                        changed = true;
                    }
                }
            }
            std::set<std::uint32_t> noreturn;
            for (const auto& [addr, fn] : out_.functions) {
                if (!returns.contains(addr)) {
                    noreturn.insert(addr);
                }
            }
            if (noreturn == noreturn_) {
                break;
            }
            noreturn_ = std::move(noreturn);
            for (auto& [addr, fn] : out_.functions) {
                fn = walk_function(image_, {fn.entry, fn.mode, fn.source}, known_, noreturn_, options_.skip).function;
            }
        }
        for (auto& [addr, fn] : out_.functions) {
            fn.noreturn = noreturn_.contains(addr);
        }
    }

    bool inside_known_code(std::uint32_t addr) const {
        for (const auto& [entry, fn] : out_.functions) {
            if (fn.insns.contains(addr)) {
                return true;
            }
        }
        return false;
    }

    bool skipped(std::uint32_t addr) const {
        for (const auto& r : options_.skip) {
            if (r.contains(addr)) {
                return true;
            }
        }
        return false;
    }

    const Image& image_;
    const DiscoveryOptions& options_;
    std::set<std::uint32_t> known_;
    std::set<std::uint32_t> noreturn_;
    std::map<std::uint32_t, Mode> modes_;
    std::deque<EntryPoint> queue_;
    std::vector<EntryPoint> candidates_;
    Discovery out_;
};

}  // namespace

Discovery discover(const Image& image, const DiscoveryOptions& options) {
    return Discoverer(image, options).run();
}

}  // namespace anygba::analysis
