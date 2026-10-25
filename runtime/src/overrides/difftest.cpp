#include "overrides/difftest.h"

#include "anygba/override.hpp"
#include "control/json.h"
#include "overrides/overrides.h"

#define TOML_EXCEPTIONS 1
#include <toml.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>

namespace anygba::difftest {

namespace fs = std::filesystem;
using control::Json;

namespace {

std::string hex32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

std::string hex_bytes(const std::uint8_t* p, std::size_t n, std::size_t max = 16) {
    std::string out;
    char buf[4];
    for (std::size_t i = 0; i < n && i < max; ++i) {
        std::snprintf(buf, sizeof buf, "%02x", p[i]);
        out += (i ? " " : "") + std::string(buf);
    }
    if (n > max) out += " ...";
    return out;
}

struct RegionInfo {
    const char* name;
    std::uint32_t base;
    std::uint32_t size;
};

RegionInfo region_info(Region r, std::uint32_t rom_size) {
    switch (r) {
    case Region::Ewram: return {"ewram", 0x02000000, Memory::kEwramSize};
    case Region::Iwram: return {"iwram", 0x03000000, Memory::kIwramSize};
    case Region::Io: return {"io", 0x04000000, Memory::kIoSize};
    case Region::Palette: return {"palette", 0x05000000, Memory::kPaletteSize};
    case Region::Vram: return {"vram", 0x06000000, Memory::kVramSize};
    case Region::Oam: return {"oam", 0x07000000, Memory::kOamSize};
    case Region::Rom: return {"rom", 0x08000000, rom_size};
    }
    return {"?", 0, 0};
}

// Writable memory as the state image holds it, by address; null outside it.
std::uint8_t* memory_at(Memory& mem, std::uint32_t addr) {
    switch (addr >> 24) {
    case 0x02: return &mem.ewram[addr & (Memory::kEwramSize - 1)];
    case 0x03: return &mem.iwram[addr & (Memory::kIwramSize - 1)];
    case 0x04: return (addr & 0xFFFFFF) < Memory::kIoSize ? &mem.io[addr & 0xFFFFFF] : nullptr;
    case 0x05: return &mem.palette[addr & (Memory::kPaletteSize - 1)];
    case 0x06: {
        std::uint32_t off = addr & 0x1FFFF;
        if (off >= Memory::kVramSize) off -= 0x8000;
        return &mem.vram[off];
    }
    case 0x07: return &mem.oam[addr & (Memory::kOamSize - 1)];
    default: return nullptr;
    }
}

void apply_pokes(Memory& mem, const std::vector<Poke>& pokes) {
    for (const Poke& p : pokes) {
        for (std::size_t i = 0; i < p.bytes.size(); ++i) {
            const std::uint32_t a = p.addr + static_cast<std::uint32_t>(i);
            std::uint8_t* dst = memory_at(mem, a);
            if (!dst) throw Error("cannot plant state data at " + hex32(a) + " (only RAM, IO, palette, VRAM and OAM)");
            *dst = p.bytes[i];
        }
    }
}

// The six memory areas a state covers, in a fixed order (recordings and comparisons).
struct Area {
    const char* name;
    std::uint32_t base;
    std::vector<std::uint8_t> Memory::*data;
};
constexpr Area kAreas[] = {
    {"ewram", 0x02000000, &Memory::ewram}, {"iwram", 0x03000000, &Memory::iwram},
    {"io", 0x04000000, &Memory::io},       {"palette", 0x05000000, &Memory::palette},
    {"vram", 0x06000000, &Memory::vram},   {"oam", 0x07000000, &Memory::oam},
};

// ---------------------------------------------------------------------------------------------
// Spec parsing

constexpr int kOrder[] = {13, 14, kCyclesSlot, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

// Generation order of a shape: sp, lr, the cycle budget, then r0-r12 except the values near another
// register, then those.
std::vector<int> generation_order(const Shape& shape) {
    std::vector<int> order;
    for (const bool near : {false, true}) {
        for (const int slot : kOrder) {
            if ((shape.values[static_cast<std::size_t>(slot)].kind == Value::Kind::Near) == near) order.push_back(slot);
        }
    }
    return order;
}

std::optional<int> slot_named(std::string_view key) {
    if (key == "sp") return 13;
    if (key == "lr") return 14;
    if (key == "cycles") return kCyclesSlot;
    if (key.size() >= 2 && key[0] == 'r') {
        int n = 0;
        for (const char ch : key.substr(1)) {
            if (ch < '0' || ch > '9') return std::nullopt;
            n = n * 10 + (ch - '0');
            if (n > 14) return std::nullopt;
        }
        if (key.size() > 2 && key[1] == '0') return std::nullopt;
        return n;
    }
    return std::nullopt;
}

std::string slot_name(int slot) {
    if (slot == 13) return "sp";
    if (slot == 14) return "lr";
    if (slot == kCyclesSlot) return "cycles";
    return "r" + std::to_string(slot);
}

class SpecReader {
public:
    explicit SpecReader(std::string source) : source_(std::move(source)) {}

    [[noreturn]] void fail(const toml::node& node, const std::string& message) const {
        throw Error(source_ + ":" + std::to_string(node.source().begin.line) + ": " + message);
    }

    std::int64_t integer(const toml::node& node, const std::string& what, std::int64_t lo, std::int64_t hi) const {
        const auto* v = node.as_integer();
        if (!v || v->get() < lo || v->get() > hi) {
            fail(node, what + " must be an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
        }
        return v->get();
    }

    std::pair<std::int64_t, std::int64_t> pair(const toml::node& node, const std::string& what, std::int64_t lo,
                                               std::int64_t hi) const {
        const auto* a = node.as_array();
        if (!a || a->size() != 2) fail(node, what + " must be [low, high]");
        const std::int64_t x = integer(*a->get(0), what, lo, hi);
        const std::int64_t y = integer(*a->get(1), what, lo, hi);
        if (x > y) fail(node, what + ": low is above high");
        return {x, y};
    }

    std::vector<Region> regions(const toml::node& node, const std::string& what) const {
        std::vector<std::string> names;
        if (const auto* s = node.as_string()) {
            names.push_back(s->get());
        } else if (const auto* a = node.as_array()) {
            for (const auto& item : *a) {
                const auto* s2 = item.as_string();
                if (!s2) fail(item, what + ": regions are strings");
                names.push_back(s2->get());
            }
        } else {
            fail(node, what + " must name a region or a list of regions");
        }
        std::vector<Region> out;
        for (const auto& n : names) {
            if (n == "ram") {
                out.push_back(Region::Ewram);
                out.push_back(Region::Iwram);
            } else if (n == "ewram") {
                out.push_back(Region::Ewram);
            } else if (n == "iwram") {
                out.push_back(Region::Iwram);
            } else if (n == "io") {
                out.push_back(Region::Io);
            } else if (n == "palette") {
                out.push_back(Region::Palette);
            } else if (n == "vram") {
                out.push_back(Region::Vram);
            } else if (n == "oam") {
                out.push_back(Region::Oam);
            } else if (n == "rom") {
                out.push_back(Region::Rom);
            } else {
                fail(node, what + ": unknown region '" + n +
                               "' (ram, ewram, iwram, io, palette, vram, oam, rom, or \"any\" alone)");
            }
        }
        if (out.empty()) fail(node, what + ": no region");
        return out;
    }

    Value value(const toml::node& node, int slot) const {
        const std::string what = slot_name(slot);
        const std::int64_t max = slot == kCyclesSlot ? std::numeric_limits<std::int32_t>::max() : 0xFFFFFFFFll;
        Value v;
        if (node.is_integer()) {
            v.kind = Value::Kind::Fixed;
            v.lo = static_cast<std::uint32_t>(integer(node, what, 0, max));
            return v;
        }
        if (const auto* s = node.as_string(); s && s->get() == "any") {
            if (slot == kCyclesSlot) fail(node, "cycles cannot be \"any\"; give a range");
            return v;
        }
        if (node.is_string() || node.is_array()) {
            v.kind = Value::Kind::Pointer;
            v.regions = regions(node, what);
            return v;
        }
        const auto* t = node.as_table();
        if (!t) fail(node, what + ": expected an integer, \"any\", a region or a table");
        static const std::set<std::string_view> kAllowed = {"value",  "range", "pointer", "string",     "buffer", "near",
                                                            "align",  "thumb", "length",  "terminator", "bytes",  "size",
                                                            "offset"};
        int kinds = 0;
        for (const auto& [key, item] : *t) {
            if (!kAllowed.contains(key.str())) fail(item, what + ": unknown key '" + std::string(key.str()) + "'");
            const std::string_view k = key.str();
            if (k == "value" || k == "range" || k == "pointer" || k == "string" || k == "buffer" || k == "near") ++kinds;
        }
        if (kinds != 1) fail(node, what + ": give exactly one of value, range, pointer, string, buffer, near");
        const auto only = [&](std::initializer_list<std::string_view> keys, const char* kind) {
            for (const auto& [key, item] : *t) {
                const std::string_view k = key.str();
                if (k == kind || k == "align" || k == "thumb") continue;
                if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
                    fail(item, what + ": '" + std::string(k) + "' does not apply to " + kind);
                }
            }
        };
        if (const toml::node* x = t->get("value")) {
            only({}, "value");
            v.kind = Value::Kind::Fixed;
            v.lo = static_cast<std::uint32_t>(integer(*x, what, 0, max));
        } else if (const toml::node* r = t->get("range")) {
            only({}, "range");
            v.kind = Value::Kind::Range;
            const auto [lo, hi] = pair(*r, what + ".range", 0, max);
            v.lo = static_cast<std::uint32_t>(lo);
            v.hi = static_cast<std::uint32_t>(hi);
        } else if (const toml::node* p = t->get("pointer")) {
            only({}, "pointer");
            v.kind = Value::Kind::Pointer;
            v.regions = regions(*p, what + ".pointer");
        } else if (const toml::node* s = t->get("string")) {
            only({"length", "terminator", "bytes"}, "string");
            v.kind = Value::Kind::String;
            v.regions = regions(*s, what + ".string");
            v.min_len = 0;
            v.max_len = 64;
            if (const toml::node* l = t->get("length")) {
                const auto [lo, hi] = pair(*l, what + ".length", 0, 0x10000);
                v.min_len = static_cast<std::uint32_t>(lo);
                v.max_len = static_cast<std::uint32_t>(hi);
            }
            if (const toml::node* term = t->get("terminator")) {
                v.terminator = static_cast<std::uint8_t>(integer(*term, what + ".terminator", 0, 255));
            }
        } else if (const toml::node* b = t->get("buffer")) {
            only({"size", "bytes"}, "buffer");
            v.kind = Value::Kind::Buffer;
            v.regions = regions(*b, what + ".buffer");
            const toml::node* size = t->get("size");
            if (!size) fail(node, what + ": a buffer needs size = [min, max]");
            const auto [lo, hi] = pair(*size, what + ".size", 0, 0x10000);
            v.min_len = static_cast<std::uint32_t>(lo);
            v.max_len = static_cast<std::uint32_t>(hi);
        } else if (const toml::node* n = t->get("near")) {
            only({"offset"}, "near");
            if (slot == kCyclesSlot) fail(*n, "cycles cannot be near a register");
            v.kind = Value::Kind::Near;
            const auto* name = n->as_string();
            const std::optional<int> target = name ? slot_named(name->get()) : std::nullopt;
            if (!target || *target == kCyclesSlot) fail(*n, what + ".near must name a register (r0-r12, sp, lr)");
            if (*target == slot) fail(*n, what + ".near cannot name " + what + " itself");
            v.near = *target;
            if (const toml::node* off = t->get("offset")) {
                const auto [lo, hi] = pair(*off, what + ".offset", -0x10000000ll, 0x10000000ll);
                v.near_lo = lo;
                v.near_hi = hi;
            }
        }
        if (const toml::node* bytes = t->get("bytes")) {
            const auto [lo, hi] = pair(*bytes, what + ".bytes", 0, 255);
            v.byte_lo = static_cast<std::uint8_t>(lo);
            v.byte_hi = static_cast<std::uint8_t>(hi);
        }
        if (v.kind == Value::Kind::String && v.byte_lo == v.byte_hi && v.byte_lo == v.terminator) {
            fail(node, what + ": the only byte value allowed is the terminator");
        }
        if ((v.kind == Value::Kind::String || v.kind == Value::Kind::Buffer) &&
            std::find(v.regions.begin(), v.regions.end(), Region::Rom) != v.regions.end()) {
            fail(node, what + ": strings and buffers are planted in writable memory, not in rom");
        }
        if (const toml::node* a = t->get("align")) {
            const std::int64_t align = integer(*a, what + ".align", 1, 0x10000);
            if ((align & (align - 1)) != 0) fail(*a, what + ".align must be a power of two");
            v.align = static_cast<std::uint32_t>(align);
        }
        if (const toml::node* th = t->get("thumb")) {
            if (!th->is_boolean()) fail(*th, what + ".thumb must be true or false");
            v.thumb = th->as_boolean()->get();
        }
        return v;
    }

    Shape shape(const toml::table& t, std::size_t number) const {
        Shape s;
        s.name = "shape" + std::to_string(number + 1);
        s.values[13].kind = Value::Kind::Range;
        s.values[13].lo = 0x03007A00;
        s.values[13].hi = 0x03007F00;
        s.values[13].align = 4;
        s.values[14].kind = Value::Kind::Pointer;
        s.values[14].regions = {Region::Rom};
        s.values[14].align = 2;
        s.values[14].thumb = true;
        s.values[kCyclesSlot].kind = Value::Kind::Fixed;
        s.values[kCyclesSlot].lo = 1000000;
        for (const auto& [key, node] : t) {
            const std::string_view k = key.str();
            if (k == "name") {
                const auto* n = node.as_string();
                if (!n || n->get().empty()) fail(node, "shape name must be a non-empty string");
                s.name = n->get();
            } else if (k == "weight") {
                s.weight = static_cast<std::uint32_t>(integer(node, "weight", 1, 1000000));
            } else if (const auto slot = slot_named(k)) {
                s.values[static_cast<std::size_t>(*slot)] = value(node, *slot);
            } else {
                fail(node, "unknown key '" + std::string(k) + "' in a shape (name, weight, r0-r12, sp, lr, cycles)");
            }
        }
        for (const auto& [key, node] : t) {
            const auto slot = slot_named(key.str());
            if (!slot) continue;
            const Value& v = s.values[static_cast<std::size_t>(*slot)];
            if (v.kind == Value::Kind::Near && s.values[static_cast<std::size_t>(v.near)].kind == Value::Kind::Near) {
                fail(node, slot_name(*slot) + ".near: " + slot_name(v.near) + " is itself near another register");
            }
        }
        return s;
    }

private:
    std::string source_;
};

// ---------------------------------------------------------------------------------------------
// Random states

std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

struct Rng {
    std::uint64_t s;
    std::uint32_t next() {
        s = mix(s);
        return static_cast<std::uint32_t>(s >> 32);
    }
    std::uint64_t below(std::uint64_t n) {
        const std::uint64_t wide = (static_cast<std::uint64_t>(next()) << 32) | next();
        return n ? wide % n : 0;
    }
};

std::uint64_t stream(std::uint64_t seed, std::uint64_t index, std::uint64_t slot) {
    return mix(mix(seed) ^ mix(index + 0x5151) ^ mix(slot * 0x632BE59BD9B4E019ull + 1));
}

// ---------------------------------------------------------------------------------------------
// Running and comparing

struct Write {
    std::uint32_t addr = 0;
    std::uint32_t value = 0;
    std::uint8_t size = 0;
    bool operator==(const Write&) const = default;
};

struct Call {
    std::uint32_t target = 0;
    std::array<std::uint32_t, 4> args{};
    std::uint32_t lr = 0;
};

struct Result {
    std::string error;
    std::array<std::uint32_t, 16> r{};
    std::uint32_t n = 0, z = 0, c = 0, v = 0, t = 0, ctrl = 0;
    std::int32_t cycles = 0;
    std::uint64_t ticks = 0;
    std::vector<Write> writes;
    std::vector<Call> calls;
};

struct Diff {
    std::string key;
    std::string text;
};

struct Verdict {
    std::vector<Diff> abi;   // the gate
    std::vector<Diff> full;  // the rest of the machine state
    std::string stop;        // both runs stopped the same way (timeout, fault): compared no further
    bool returned = false;   // both returned to the caller's lr
};

std::string describe_write(const Write& w) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "str%d [0x%08x] = 0x%0*x", w.size * 8, w.addr, w.size * 2, w.value);
    return buf;
}

std::string describe_call(const Call& c, bool with_lr) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "0x%08x(r0=0x%08x, r1=0x%08x, r2=0x%08x, r3=0x%08x)", c.target, c.args[0],
                  c.args[1], c.args[2], c.args[3]);
    return std::string(buf) + (with_lr ? " lr=" + hex32(c.lr) : "");
}

std::string flags_text(const Result& r) {
    std::string s;
    s += r.n ? 'N' : 'n';
    s += r.z ? 'Z' : 'z';
    s += r.c ? 'C' : 'c';
    s += r.v ? 'V' : 'v';
    return s;
}

std::string first_difference(const std::vector<Write>& a, const std::vector<Write>& b) {
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    return "original " + std::to_string(a.size()) + " writes, override " + std::to_string(b.size()) +
           "; first difference at #" + std::to_string(i) + ": original " +
           (i < a.size() ? describe_write(a[i]) : "(none)") + ", override " +
           (i < b.size() ? describe_write(b[i]) : "(none)");
}

class Harness {
public:
    Harness(System& sys, const anygba_game& game, std::uint32_t index, const Spec& spec, std::uint64_t tick_limit)
        : sys_(sys), game_(game), index_(index), entry_(game.overrides[index]), spec_(spec), tick_limit_(tick_limit),
          was_on_(game.override_on[index]) {
        // The f_XXXXXXXX switch: with override_on set, it runs the override as every call path does.
        for (std::uint32_t i = 0; i < game.function_count; ++i) {
            if (game.functions[i].addr == entry_.addr) wrapper_ = game.functions[i].fn;
        }
        if (!wrapper_) throw Error("the generated program has no function at " + hex32(entry_.addr));
        hits_.assign(entry_.block_hits ? entry_.block_count : 0, 0);

        std::set<std::uint32_t> entries;
        for (std::uint32_t i = 0; i < game.function_count; ++i) entries.insert(game.functions[i].addr);
        sys_.set_breakpoints(std::move(entries));
        sys_.on_breakpoint = [this](System& s, std::uint32_t addr) {
            if (!current_) return;
            const anygba_cpu& c = s.cpu();
            if (addr == entry_.addr && c.ret_depth == 0) return;  // the function under test itself
            current_->calls.push_back({addr, {c.r[0], c.r[1], c.r[2], c.r[3]}, c.r[14]});
        };
        sys_.on_watch = [this](System&, const System::Access& a) {
            // The address the store reaches (stores pass it unaligned; SRAM's byte store keeps it).
            if (current_ && a.write) current_->writes.push_back({a.addr & ~(a.size - 1u), a.value, a.size});
        };
        sys_.set_watched_regions(0xFFFF);  // every access takes the bus, so every write is seen
    }

    ~Harness() {
        sys_.on_breakpoint = nullptr;
        sys_.on_watch = nullptr;
        sys_.set_watched_regions(0);
        sys_.set_breakpoints({});
        sys_.isolate_ticks(false);
        game_.override_on[index_] = was_on_;
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    void set_base(std::vector<std::uint8_t> snapshot) { base_ = std::move(snapshot); }

    // Runs both sides from `state`. Coverage counts only when `count_coverage`.
    Verdict check(const State& state, bool count_coverage) {
        Result a, b;
        run_side(state, false, a, count_coverage);
        snapshot_memory();
        run_side(state, true, b, false);
        return compare(state, a, b);
    }

    const std::vector<std::uint64_t>& hits() const { return hits_; }

private:
    void run_side(const State& state, bool override_side, Result& out, bool count_coverage) {
        sys_.restore(base_);
        apply_pokes(sys_.memory(), state.pokes);
        anygba_cpu& c = sys_.cpu();
        for (int i = 0; i < 15; ++i) c.r[i] = state.r[static_cast<std::size_t>(i)];
        c.r[15] = entry_.addr;
        c.n = (state.flags >> 31) & 1u;
        c.z = (state.flags >> 30) & 1u;
        c.c = (state.flags >> 29) & 1u;
        c.v = (state.flags >> 28) & 1u;
        c.t = entry_.thumb;
        c.cycles = state.cycles;
        c.rom_patched = 1;  // generated functions report their entries (calls) to on_breakpoint
        game_.override_on[index_] = override_side ? 1 : 0;
        if (entry_.block_hits) std::fill(entry_.block_hits, entry_.block_hits + entry_.block_count, 0u);
        current_ = &out;
        sys_.isolate_ticks(true, tick_limit_);
        out.error = sys_.call_isolated(override_side ? wrapper_ : entry_.original);
        out.ticks = sys_.isolated_ticks();
        sys_.isolate_ticks(false);
        current_ = nullptr;
        std::copy(std::begin(c.r), std::end(c.r), out.r.begin());
        out.n = c.n;
        out.z = c.z;
        out.c = c.c;
        out.v = c.v;
        out.t = c.t;
        out.ctrl = c.ctrl;
        out.cycles = c.cycles;
        if (count_coverage && entry_.block_hits) {
            for (std::uint32_t i = 0; i < entry_.block_count; ++i) hits_[i] += entry_.block_hits[i];
        }
    }

    void snapshot_memory() {
        Memory& mem = sys_.memory();
        memory_.clear();
        for (const Area& area : kAreas) {
            const auto& d = mem.*area.data;
            memory_.insert(memory_.end(), d.begin(), d.end());
        }
    }

    // Canonical address for the dead-stack and ignore checks (RAM mirrors folded).
    static std::uint32_t canonical(std::uint32_t a) {
        if ((a >> 24) == 0x02) return 0x02000000u | (a & (Memory::kEwramSize - 1));
        if ((a >> 24) == 0x03) return 0x03000000u | (a & (Memory::kIwramSize - 1));
        return a;
    }

    bool ignored(std::uint32_t a) const {
        a = canonical(a);
        for (const auto& [lo, hi] : spec_.ignore_memory) {
            if (a >= lo && a < hi) return true;
        }
        return false;
    }

    bool dead(std::uint32_t a) const {
        const std::uint32_t sp = canonical(sp0_);
        if ((sp >> 24) != 0x02 && (sp >> 24) != 0x03) return false;
        a = canonical(a);
        return a < sp && a >= sp - spec_.dead_stack;
    }

    Verdict compare(const State& state, const Result& a, const Result& b) {
        Verdict v;
        sp0_ = state.r[13];
        if (!a.error.empty() || !b.error.empty()) {
            if (a.error == b.error) {
                v.stop = a.error;
            } else {
                v.abi.push_back({"error", "original " + (a.error.empty() ? std::string("returned") : a.error) +
                                              "; override " + (b.error.empty() ? std::string("returned") : b.error)});
            }
            return v;
        }
        const std::uint32_t expected = state.r[14] & ~1u;
        v.returned = (a.r[15] & ~1u) == expected && (b.r[15] & ~1u) == expected;
        const auto reg = [&](int r, std::vector<Diff>& out) {
            if (a.r[static_cast<std::size_t>(r)] != b.r[static_cast<std::size_t>(r)]) {
                const std::string name = slot_name(r);
                out.push_back({name, name + ": original " + hex32(a.r[static_cast<std::size_t>(r)]) + ", override " +
                                         hex32(b.r[static_cast<std::size_t>(r)])});
            }
        };
        for (int r : {0, 4, 5, 6, 7, 8, 9, 10, 11, 13}) reg(r, v.abi);
        if (a.r[15] != b.r[15] || a.t != b.t) {
            v.abi.push_back({"return", "returns to: original " + hex32(a.r[15]) + (a.t ? " thumb" : " arm") +
                                           ", override " + hex32(b.r[15]) + (b.t ? " thumb" : " arm")});
        }
        for (int r : {1, 2, 3, 12, 14}) reg(r, v.full);
        if (a.n != b.n || a.z != b.z || a.c != b.c || a.v != b.v) {
            v.full.push_back({"flags", "flags: original " + flags_text(a) + ", override " + flags_text(b)});
        }
        if (a.ctrl != b.ctrl) {
            v.full.push_back({"mode", "CPSR mode bits: original " + hex32(a.ctrl) + ", override " + hex32(b.ctrl)});
        }
        if (a.ticks != b.ticks) {
            v.full.push_back({"ticks", "ticks (scanlines of cycles): original " + std::to_string(a.ticks) +
                                           ", override " + std::to_string(b.ticks)});
        }
        if (a.cycles != b.cycles || a.ticks != b.ticks) {
            const std::int64_t used_a = static_cast<std::int64_t>(a.ticks) * 1232 + state.cycles - a.cycles;
            const std::int64_t used_b = static_cast<std::int64_t>(b.ticks) * 1232 + state.cycles - b.cycles;
            if (used_a != used_b) {
                v.full.push_back({"cycles", "cycles used: original " + std::to_string(used_a) + ", override " +
                                                std::to_string(used_b)});
            }
        }
        compare_writes(a.writes, b.writes, v);
        compare_calls(a.calls, b.calls, v);
        compare_memory(v);
        return v;
    }

    void compare_writes(const std::vector<Write>& a, const std::vector<Write>& b, Verdict& v) const {
        std::vector<Write> io_a, io_b, live_a, live_b, all_a, all_b;
        const auto split = [&](const std::vector<Write>& in, std::vector<Write>& io, std::vector<Write>& live,
                               std::vector<Write>& all) {
            for (const Write& w : in) {
                if (ignored(w.addr)) continue;
                all.push_back(w);
                if ((w.addr >> 24) == 0x04) {
                    io.push_back(w);
                } else if (!dead(w.addr)) {
                    live.push_back(w);
                }
            }
            // Order matters per address, not across addresses (IO keeps its global order).
            std::stable_sort(live.begin(), live.end(), [](const Write& x, const Write& y) { return x.addr < y.addr; });
        };
        split(a, io_a, live_a, all_a);
        split(b, io_b, live_b, all_b);
        if (io_a != io_b) v.abi.push_back({"io-writes", "IO writes: " + first_difference(io_a, io_b)});
        if (live_a != live_b) v.abi.push_back({"writes", "memory writes (by address): " + first_difference(live_a, live_b)});
        if (all_a != all_b && io_a == io_b && live_a == live_b) {
            v.full.push_back({"write-order", "writes in order, dead stack included: " + first_difference(all_a, all_b)});
        }
    }

    static void compare_calls(const std::vector<Call>& a, const std::vector<Call>& b, Verdict& v) {
        std::size_t i = 0;
        while (i < a.size() && i < b.size() && a[i].target == b[i].target && a[i].args == b[i].args) ++i;
        if (i < a.size() || i < b.size()) {
            v.abi.push_back({"calls", "guest calls: original " + std::to_string(a.size()) + ", override " +
                                          std::to_string(b.size()) + "; first difference at #" + std::to_string(i) +
                                          ": original " + (i < a.size() ? describe_call(a[i], false) : "(none)") +
                                          ", override " + (i < b.size() ? describe_call(b[i], false) : "(none)")});
            return;
        }
        for (std::size_t k = 0; k < a.size(); ++k) {
            if (a[k].lr != b[k].lr) {
                v.full.push_back({"call-lr", "lr of call #" + std::to_string(k) + " to " + hex32(a[k].target) +
                                                 ": original " + hex32(a[k].lr) + ", override " + hex32(b[k].lr)});
                return;
            }
        }
    }

    void compare_memory(Verdict& v) const {
        Memory& mem = sys_.memory();
        std::size_t offset = 0;
        std::vector<std::string> live, stack;
        std::size_t live_bytes = 0, stack_bytes = 0;
        for (const Area& area : kAreas) {
            const auto& now = mem.*area.data;
            const std::uint8_t* before = memory_.data() + offset;
            offset += now.size();
            if (std::memcmp(before, now.data(), now.size()) == 0) continue;
            for (std::size_t i = 0; i < now.size();) {
                const std::uint32_t addr = area.base + static_cast<std::uint32_t>(i);
                if (before[i] == now[i] || ignored(addr)) {
                    ++i;
                    continue;
                }
                const bool in_stack = dead(addr);
                std::size_t j = i;
                while (j < now.size() && before[j] != now[j] && !ignored(area.base + static_cast<std::uint32_t>(j)) &&
                       dead(area.base + static_cast<std::uint32_t>(j)) == in_stack) {
                    ++j;
                }
                (in_stack ? stack_bytes : live_bytes) += j - i;
                auto& list = in_stack ? stack : live;
                if (list.size() < 4) {
                    list.push_back(hex32(addr) + "-" + hex32(addr + static_cast<std::uint32_t>(j - i - 1)) + " (" +
                                   area.name + ", " + std::to_string(j - i) + " bytes): original " +
                                   hex_bytes(before + i, j - i) + ", override " + hex_bytes(now.data() + i, j - i));
                }
                i = j;
            }
        }
        const auto join = [](const std::vector<std::string>& parts) {
            std::string s;
            for (const auto& p : parts) s += (s.empty() ? "" : "; ") + p;
            return s;
        };
        if (live_bytes) {
            v.abi.push_back({"memory", "memory differs in " + std::to_string(live_bytes) + " bytes: " + join(live)});
        }
        if (stack_bytes) {
            v.full.push_back({"stack", "dead stack differs in " + std::to_string(stack_bytes) + " bytes: " +
                                           join(stack)});
        }
    }

    System& sys_;
    const anygba_game& game_;
    std::uint32_t index_;
    const anygba_override_entry& entry_;
    const Spec& spec_;
    std::uint64_t tick_limit_;
    std::uint8_t was_on_;
    anygba_fn wrapper_ = nullptr;
    std::vector<std::uint8_t> base_;
    std::vector<std::uint8_t> memory_;
    std::vector<std::uint64_t> hits_;
    Result* current_ = nullptr;
    std::uint32_t sp0_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Report

std::string describe_state(const State& s) {
    std::string out;
    for (int r = 0; r < 15; ++r) {
        if (r == 13 || r == 14) continue;
        out += slot_name(r) + "=" + hex32(s.r[static_cast<std::size_t>(r)]) + " ";
    }
    out += "sp=" + hex32(s.r[13]) + " lr=" + hex32(s.r[14]) + " flags=";
    out += (s.flags >> 31) & 1 ? 'N' : 'n';
    out += (s.flags >> 30) & 1 ? 'Z' : 'z';
    out += (s.flags >> 29) & 1 ? 'C' : 'c';
    out += (s.flags >> 28) & 1 ? 'V' : 'v';
    out += " cycles=" + std::to_string(s.cycles);
    for (const Poke& p : s.pokes) {
        out += "\n      memory " + hex32(p.addr) + " (" + std::to_string(p.bytes.size()) +
               " bytes): " + hex_bytes(p.bytes.data(), p.bytes.size(), 32);
    }
    return out;
}

Json state_json(const State& s) {
    Json j = Json::object();
    Json regs = Json::array();
    for (int r = 0; r < 15; ++r) regs.push_back(hex32(s.r[static_cast<std::size_t>(r)]));
    j["r"] = std::move(regs);
    j["flags"] = hex32(s.flags);
    j["cycles"] = static_cast<long long>(s.cycles);
    Json pokes = Json::array();
    for (const Poke& p : s.pokes) {
        Json pj = Json::object();
        pj["addr"] = hex32(p.addr);
        pj["bytes"] = control::hex_encode(p.bytes.data(), p.bytes.size());
        pokes.push_back(std::move(pj));
    }
    j["memory"] = std::move(pokes);
    return j;
}

Json diffs_json(const std::vector<Diff>& diffs) {
    Json a = Json::array();
    for (const Diff& d : diffs) {
        Json j = Json::object();
        j["what"] = d.key;
        j["text"] = d.text;
        a.push_back(std::move(j));
    }
    return a;
}

void write_text_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) throw Error("cannot write " + path.string());
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

constexpr char kMagic[8] = {'A', 'G', 'D', 'T', 'R', 'E', 'C', '1'};

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

class Cursor {
public:
    Cursor(const std::vector<std::uint8_t>& data, std::string source) : data_(data), source_(std::move(source)) {}
    std::uint32_t u32() {
        need(4);
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data_[pos_ + static_cast<std::size_t>(i)]) << (8 * i);
        pos_ += 4;
        return v;
    }
    std::vector<std::uint8_t> bytes(std::size_t n) {
        need(n);
        std::vector<std::uint8_t> v(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                    data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return v;
    }
    bool at_end() const { return pos_ == data_.size(); }

private:
    void need(std::size_t n) const {
        if (n > data_.size() - pos_) throw Error(source_ + ": recorded states file is truncated");
    }
    const std::vector<std::uint8_t>& data_;
    std::string source_;
    std::size_t pos_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------------------------

std::optional<Spec> parse_spec(std::string_view text, const std::string& source, std::string_view name,
                               std::uint32_t addr) {
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        throw Error(source + ":" + std::to_string(e.source().begin.line) + ": " + std::string(e.description()));
    }
    const toml::table* table = nullptr;
    for (const auto& [key, node] : root) {
        const std::string k(key.str());
        bool match = k == name;
        if (!match && k.size() > 2 && k[0] == '0' && (k[1] == 'x' || k[1] == 'X')) {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(k.c_str(), &end, 16);
            match = *end == '\0' && v == addr;
        }
        if (match) {
            table = node.as_table();
            if (!table) throw Error(source + ": [" + k + "] must be a table");
            break;
        }
    }
    if (!table) return std::nullopt;
    const SpecReader r(source);
    Spec spec;
    for (const auto& [key, node] : *table) {
        const std::string_view k = key.str();
        if (k == "dead_stack") {
            spec.dead_stack = static_cast<std::uint32_t>(r.integer(node, "dead_stack", 0, 0x8000));
        } else if (k == "tick_limit") {
            spec.tick_limit = static_cast<std::uint64_t>(r.integer(node, "tick_limit", 1, 100000000));
        } else if (k == "min_returned") {
            const std::optional<double> share = node.value<double>();
            if (!share || !(*share >= 0.0 && *share <= 1.0)) r.fail(node, "min_returned must be a number in 0..1");
            spec.min_returned = *share;
        } else if (k == "ignore_memory") {
            const auto* a = node.as_array();
            if (!a) r.fail(node, "ignore_memory must be a list of [begin, end) address pairs");
            for (const auto& item : *a) {
                const auto [lo, hi] = r.pair(item, "ignore_memory", 0, 0xFFFFFFFFll);
                spec.ignore_memory.emplace_back(static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(hi));
            }
        } else if (k == "shape") {
            const auto* a = node.as_array();
            if (!a) r.fail(node, "shape must be an array of tables ([[" + std::string(name) + ".shape]])");
            for (const auto& item : *a) {
                const auto* t = item.as_table();
                if (!t) r.fail(item, "each shape must be a table");
                spec.shapes.push_back(r.shape(*t, spec.shapes.size()));
            }
        } else {
            r.fail(node, "unknown key '" + std::string(k) +
                            "' (dead_stack, tick_limit, min_returned, ignore_memory, shape)");
        }
    }
    if (spec.shapes.empty()) {
        throw Error(source + ": [" + std::string(name) + "] needs at least one [[" + std::string(name) + ".shape]]");
    }
    return spec;
}

Status classify(std::uint64_t total, std::uint64_t failed, std::uint64_t returned, double min_returned) {
    if (failed) return Status::Fail;
    const double share = total ? static_cast<double>(returned) / static_cast<double>(total) : 0.0;
    return share >= min_returned ? Status::Pass : Status::Insufficient;
}

const char* status_name(Status status) {
    switch (status) {
    case Status::Pass: return "pass";
    case Status::Fail: return "fail";
    case Status::Insufficient: return "insufficient";
    }
    return "?";
}

int exit_code(Status status) {
    switch (status) {
    case Status::Pass: return 0;
    case Status::Fail: return 1;
    case Status::Insufficient: return 3;
    }
    return 2;
}

std::size_t pick_shape(const Spec& spec, std::uint64_t seed, std::uint64_t index) {
    std::uint64_t total = 0;
    for (const Shape& s : spec.shapes) total += s.weight;
    Rng rng{stream(seed, index, 1000)};
    std::uint64_t x = rng.below(total);
    for (std::size_t i = 0; i < spec.shapes.size(); ++i) {
        if (x < spec.shapes[i].weight) return i;
        x -= spec.shapes[i].weight;
    }
    return spec.shapes.size() - 1;
}

Generated generate(const Spec& spec, std::size_t shape, std::uint64_t seed, std::uint64_t index, const Params& forced,
                   std::uint32_t rom_size) {
    const Shape& sh = spec.shapes.at(shape);
    Generated g;
    std::array<std::uint64_t, kSlots> val{};
    for (const int slot : generation_order(sh)) {
        const auto s = static_cast<std::size_t>(slot);
        const Value& v = sh.values[s];
        Rng rng{stream(seed, index, s)};
        std::uint64_t out = 0;
        const auto pick_region = [&]() {
            const RegionInfo info = region_info(v.regions[rng.below(v.regions.size())], rom_size);
            if (info.size == 0) throw Error(slot_name(slot) + ": region " + info.name + " is empty");
            return info;
        };
        switch (v.kind) {
        case Value::Kind::Any: {
            const std::uint64_t drawn = slot == kCyclesSlot ? (rng.next() & 0x7FFFFFFFu) : rng.next();
            out = forced[s] ? *forced[s] : drawn;
            g.params[s] = out;
            break;
        }
        case Value::Kind::Fixed: out = v.lo; break;
        case Value::Kind::Range: {
            const std::uint64_t drawn = v.lo + rng.below(static_cast<std::uint64_t>(v.hi) - v.lo + 1);
            out = forced[s] ? std::clamp<std::uint64_t>(*forced[s], v.lo, v.hi) : drawn;
            out &= ~static_cast<std::uint64_t>(v.align - 1);
            if (out < v.lo) out += v.align;
            g.params[s] = out;
            break;
        }
        case Value::Kind::Pointer: {
            const RegionInfo info = pick_region();
            out = static_cast<std::uint32_t>(info.base + rng.below(info.size)) & ~(v.align - 1u);
            break;
        }
        case Value::Kind::String:
        case Value::Kind::Buffer: {
            const bool string = v.kind == Value::Kind::String;
            const std::uint64_t drawn = v.min_len + rng.below(static_cast<std::uint64_t>(v.max_len) - v.min_len + 1);
            const std::uint64_t len = forced[s] ? std::clamp<std::uint64_t>(*forced[s], v.min_len, v.max_len) : drawn;
            g.params[s] = len;
            const RegionInfo info = pick_region();
            // Placed for the longest length, so a shorter forced length keeps the address.
            const std::uint64_t need = std::uint64_t{v.max_len} + (string ? 1 : 0);
            if (need > info.size) throw Error(slot_name(slot) + ": " + std::to_string(need) + " bytes do not fit in " + info.name);
            const std::uint32_t at = static_cast<std::uint32_t>(info.base + rng.below(info.size - need + 1)) & ~(v.align - 1u);
            Poke p;
            p.addr = at;
            const std::uint32_t span = static_cast<std::uint32_t>(v.byte_hi) - v.byte_lo + 1;
            for (std::uint64_t i = 0; i < len; ++i) {
                std::uint32_t b = v.byte_lo + static_cast<std::uint32_t>(rng.below(span));
                if (string && b == v.terminator) b = b == v.byte_hi ? v.byte_lo : b + 1;
                p.bytes.push_back(static_cast<std::uint8_t>(b));
            }
            if (string) p.bytes.push_back(v.terminator);
            g.state.pokes.push_back(std::move(p));
            out = at;
            break;
        }
        case Value::Kind::Near: {
            const std::uint64_t span = static_cast<std::uint64_t>(v.near_hi - v.near_lo) + 1;
            const std::int64_t off = v.near_lo + static_cast<std::int64_t>(rng.below(span));
            out = static_cast<std::uint32_t>(val[static_cast<std::size_t>(v.near)] + static_cast<std::uint64_t>(off));
            break;
        }
        }
        if (v.thumb) out |= 1u;
        val[s] = out;
    }
    for (std::size_t r = 0; r < 15; ++r) g.state.r[r] = static_cast<std::uint32_t>(val[r]);
    g.state.cycles = static_cast<std::int32_t>(val[kCyclesSlot] & 0x7FFFFFFFu);
    Rng flags{stream(seed, index, 2000)};
    g.state.flags = (flags.next() & 0xFu) << 28;
    return g;
}

void write_recording(const fs::path& path, const Recording& rec) {
    std::vector<std::uint8_t> out(std::begin(kMagic), std::end(kMagic));
    put32(out, 1);
    put32(out, rec.entry);
    put32(out, rec.thumb);
    put32(out, static_cast<std::uint32_t>(rec.base.size()));
    out.insert(out.end(), rec.base.begin(), rec.base.end());
    put32(out, static_cast<std::uint32_t>(rec.states.size()));
    for (const State& s : rec.states) {
        for (const std::uint32_t r : s.r) put32(out, r);
        put32(out, s.flags);
        put32(out, static_cast<std::uint32_t>(s.cycles));
        put32(out, static_cast<std::uint32_t>(s.pokes.size()));
        for (const Poke& p : s.pokes) {
            put32(out, p.addr);
            put32(out, static_cast<std::uint32_t>(p.bytes.size()));
            out.insert(out.end(), p.bytes.begin(), p.bytes.end());
        }
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    f.close();
    if (!f) throw Error("cannot write " + path.string());
}

Recording read_recording(const fs::path& path) {
    const std::vector<std::uint8_t> data = read_file(path);
    const std::string source = path.string();
    if (data.size() < sizeof kMagic || std::memcmp(data.data(), kMagic, sizeof kMagic) != 0) {
        throw Error(source + ": not a recorded states file (anygba --record-entry)");
    }
    const std::vector<std::uint8_t> rest(data.begin() + sizeof kMagic, data.end());
    Cursor c(rest, source);
    if (const std::uint32_t version = c.u32(); version != 1) {
        throw Error(source + ": recorded states format " + std::to_string(version) + " is not supported");
    }
    Recording rec;
    rec.entry = c.u32();
    rec.thumb = c.u32();
    rec.base = c.bytes(c.u32());
    const std::uint32_t count = c.u32();
    for (std::uint32_t i = 0; i < count; ++i) {
        State s;
        for (auto& r : s.r) r = c.u32();
        s.flags = c.u32();
        s.cycles = static_cast<std::int32_t>(c.u32());
        const std::uint32_t pokes = c.u32();
        for (std::uint32_t k = 0; k < pokes; ++k) {
            Poke p;
            p.addr = c.u32();
            p.bytes = c.bytes(c.u32());
            s.pokes.push_back(std::move(p));
        }
        rec.states.push_back(std::move(s));
    }
    if (!c.at_end()) throw Error(source + ": recorded states file has trailing data");
    return rec;
}

std::optional<std::uint32_t> find_override(const anygba_game& game, std::string_view function) {
    std::optional<std::uint32_t> addr;
    if (function.size() > 2 && function[0] == '0' && (function[1] == 'x' || function[1] == 'X')) {
        const std::string text(function);
        char* end = nullptr;
        const unsigned long long v = std::strtoull(text.c_str(), &end, 16);
        if (*end == '\0' && v <= 0xFFFFFFFFull) addr = static_cast<std::uint32_t>(v);
    }
    for (std::uint32_t i = 0; i < game.override_count; ++i) {
        const anygba_override_entry& e = game.overrides[i];
        if (function == e.name || (addr && *addr == e.addr)) return i;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------

Recorder::Recorder(System& sys, const anygba_game& game, std::string_view function, std::uint64_t count, fs::path out)
    : sys_(sys), count_(count), out_(std::move(out)) {
    if (count_ == 0) throw Error("--count must be at least 1");
    if (const auto index = find_override(game, function)) {
        recording_.entry = game.overrides[*index].addr;
        if (game.override_on[*index]) {
            game.override_on[*index] = 0;  // the translation's entry is where states are taken
            std::printf("record: override %s switched off for recording\n", game.overrides[*index].name);
        }
    } else {
        const std::string text(function);
        char* end = nullptr;
        const unsigned long long v = std::strtoull(text.c_str(), &end, 0);
        if (text.empty() || *end != '\0' || v > 0xFFFFFFFFull) {
            throw Error("--record-entry " + text + ": no override has that name; give the entry address (0x...)");
        }
        recording_.entry = static_cast<std::uint32_t>(v) & ~1u;
    }
    sys_.set_breakpoints({recording_.entry});
    sys_.on_breakpoint = [this](System&, std::uint32_t addr) {
        if (addr == recording_.entry) hit();
    };
}

void Recorder::hit() {
    if (recording_.states.size() >= count_) return;
    Memory& mem = sys_.memory();
    if (recording_.states.empty()) {
        recording_.base = sys_.save_state();
        recording_.thumb = sys_.cpu().t;
        for (const Area& area : kAreas) base_memory_.push_back(mem.*area.data);
    }
    const anygba_cpu& c = sys_.cpu();
    State s;
    std::copy(std::begin(c.r), std::end(c.r), s.r.begin());
    s.r[15] = recording_.entry;
    s.flags = (c.n << 31) | (c.z << 30) | (c.c << 29) | (c.v << 28);
    s.cycles = c.cycles;
    for (std::size_t k = 0; k < std::size(kAreas); ++k) {
        const auto& now = mem.*kAreas[k].data;
        const auto& base = base_memory_[k];
        for (std::size_t i = 0; i < now.size();) {
            if (now[i] == base[i]) {
                ++i;
                continue;
            }
            // Runs of changes closer than 16 bytes share one chunk.
            std::size_t j = i + 1, last = i;
            while (j < now.size() && j - last <= 16) {
                if (now[j] != base[j]) last = j;
                ++j;
            }
            Poke p;
            p.addr = kAreas[k].base + static_cast<std::uint32_t>(i);
            p.bytes.assign(now.begin() + static_cast<std::ptrdiff_t>(i), now.begin() + static_cast<std::ptrdiff_t>(last + 1));
            s.pokes.push_back(std::move(p));
            i = last + 1;
        }
    }
    recording_.states.push_back(std::move(s));
    if (recording_.states.size() >= count_) {
        // Inside generated code: report errors through the runtime, not as C++ exceptions.
        std::string message, error;
        try {
            message = finish();
        } catch (const std::exception& ex) {
            error = ex.what();
        }
        if (!error.empty()) sys_.fail(error);
        std::printf("%s\n", message.c_str());
        sys_.stop();
    }
}

std::string Recorder::finish() {
    if (written_) return "";
    if (recording_.states.empty()) {
        written_ = true;
        return "record: " + hex32(recording_.entry) + " was not called; nothing written";
    }
    write_recording(out_, recording_);
    written_ = true;
    return "record: " + std::to_string(recording_.states.size()) + " entry states of " + hex32(recording_.entry) +
           " written to " + out_.string();
}

// ---------------------------------------------------------------------------------------------

int run(System& sys, const anygba_game& game, const Options& options) {
    const auto found = find_override(game, options.function);
    if (!found) {
        std::string names;
        for (std::uint32_t i = 0; i < game.override_count; ++i) {
            names += (names.empty() ? "" : ", ") + std::string(game.overrides[i].name);
        }
        throw Error("difftest: no override named " + options.function + " in this program (overrides: " +
                    (names.empty() ? "none" : names) + ")");
    }
    const std::uint32_t index = *found;
    const anygba_override_entry& e = game.overrides[index];
    std::string impl_name = e.impl;
    if (options.impl) {
        const guest::Impl impl = overrides::find_impl(*options.impl);
        if (!impl) throw Error("difftest: no implementation named " + *options.impl + " is compiled into this program");
        overrides::set_impl(index, impl);
        impl_name = *options.impl;
    }

    Spec spec;
    bool have_spec = false;
    if (options.spec) {
        const std::vector<std::uint8_t> bytes = read_file(*options.spec);
        const std::string text(bytes.begin(), bytes.end());
        if (auto parsed = parse_spec(text, options.spec->string(), e.name, e.addr)) {
            spec = std::move(*parsed);
            have_spec = true;
        } else if (options.states > 0) {
            throw Error(options.spec->string() + ": no [" + std::string(e.name) + "] table for this function");
        }
    }
    const std::uint64_t random_states = have_spec ? options.states : 0;
    if (random_states == 0 && options.recorded.empty()) {
        throw Error("difftest: nothing to test; give an argument spec (--spec) for random states or --recorded FILE");
    }
    const std::uint64_t tick_limit = options.tick_limit ? options.tick_limit : spec.tick_limit;

    std::vector<std::uint8_t> random_base;
    if (random_states) {
        if (options.base_state) sys.load_state(read_file(*options.base_state));
        random_base = sys.snapshot();
    }
    struct RecordedSet {
        std::string file;
        std::vector<std::uint8_t> base;
        std::vector<State> states;
    };
    std::vector<RecordedSet> recorded;
    for (const auto& path : options.recorded) {
        Recording rec = read_recording(path);
        if (rec.entry != e.addr) {
            throw Error(path.string() + ": states recorded at " + hex32(rec.entry) + ", not at " + hex32(e.addr));
        }
        sys.load_state(rec.base);
        recorded.push_back({path.string(), sys.snapshot(), std::move(rec.states)});
    }

    std::printf("difftest %s at %s %s: translation vs %s\n", e.name, hex32(e.addr).c_str(), e.thumb ? "thumb" : "arm",
                impl_name.c_str());

    Harness h(sys, game, index, spec, tick_limit);
    struct Tally {
        std::string name;
        std::uint64_t states = 0;
        std::uint64_t failed = 0;
        std::uint64_t stopped = 0;  // both runs stopped the same way
    };
    std::vector<Tally> tallies;
    for (const Shape& s : spec.shapes) tallies.push_back({s.name});
    if (random_states == 0) tallies.clear();
    const std::size_t recorded_tally = tallies.size();
    if (!recorded.empty()) tallies.push_back({"recorded"});

    std::map<std::string, std::uint64_t> abi_counts, full_counts, stops;
    std::uint64_t total = 0, failed = 0, full_identical = 0, returned = 0;
    struct Failure {
        std::uint64_t number = 0;
        bool random = true;
        std::size_t shape = 0;
        std::uint64_t index = 0;   // random: state index; recorded: position in its file
        std::size_t set = 0;       // recorded: which file
        State state;
        Verdict verdict;
    };
    std::optional<Failure> first;

    const auto tally = [&](std::size_t t, const Verdict& v, const auto& make_failure) {
        ++total;
        ++tallies[t].states;
        if (v.returned) ++returned;
        if (!v.stop.empty()) {
            ++stops[v.stop];
            ++tallies[t].stopped;
        }
        for (const Diff& d : v.abi) ++abi_counts[d.key];
        for (const Diff& d : v.full) ++full_counts[d.key];
        if (v.abi.empty() && v.full.empty()) ++full_identical;
        if (!v.abi.empty()) {
            ++failed;
            ++tallies[t].failed;
            if (!first) first = make_failure();
        }
    };

    h.set_base(random_base);
    for (std::uint64_t i = 0; i < random_states; ++i) {
        const std::size_t shape = pick_shape(spec, options.seed, i);
        Generated g = generate(spec, shape, options.seed, i, Params{}, sys.memory().rom_size);
        const Verdict v = h.check(g.state, true);
        tally(shape, v, [&] { return Failure{total, true, shape, i, 0, g.state, v}; });
    }
    for (std::size_t k = 0; k < recorded.size(); ++k) {
        h.set_base(recorded[k].base);
        for (std::size_t i = 0; i < recorded[k].states.size(); ++i) {
            const Verdict v = h.check(recorded[k].states[i], true);
            tally(recorded_tally, v, [&] { return Failure{total, false, 0, i, k, recorded[k].states[i], v}; });
        }
    }

    // Shrink the first failing state while it still fails at the ABI level.
    std::optional<State> minimised;
    std::optional<Verdict> minimised_verdict;
    if (first) {
        int budget = 400;
        if (first->random) {
            h.set_base(random_base);
            const Shape& shape = spec.shapes[first->shape];
            Params params = generate(spec, first->shape, options.seed, first->index, Params{}, sys.memory().rom_size).params;
            const auto attempt = [&](const Params& p) -> bool {
                if (--budget < 0) return false;
                const Generated g = generate(spec, first->shape, options.seed, first->index, p, sys.memory().rom_size);
                Verdict v = h.check(g.state, false);
                if (v.abi.empty()) return false;
                minimised = g.state;
                minimised_verdict = std::move(v);
                return true;
            };
            for (const int slot : kOrder) {
                const auto s = static_cast<std::size_t>(slot);
                if (!params[s] || slot == kCyclesSlot || slot == 13 || slot == 14) continue;
                const Value& v = shape.values[s];
                if (v.kind == Value::Kind::Any) {
                    if (*params[s] == 0) continue;
                    Params p = params;
                    p[s] = 0;
                    if (attempt(p)) params = p;
                    continue;
                }
                std::uint64_t lo = v.kind == Value::Kind::Range ? v.lo : v.min_len;
                std::uint64_t hi = *params[s];
                while (lo < hi && budget > 0) {
                    const std::uint64_t mid = lo + (hi - lo) / 2;
                    Params p = params;
                    p[s] = mid;
                    if (attempt(p)) {
                        hi = mid;
                    } else {
                        lo = mid + 1;
                    }
                }
                params[s] = hi;
            }
            // The last attempt may have passed: regenerate from the parameters kept.
            const Generated g = generate(spec, first->shape, options.seed, first->index, params, sys.memory().rom_size);
            Verdict v = h.check(g.state, false);
            if (!v.abi.empty()) {
                minimised = g.state;
                minimised_verdict = std::move(v);
            }
        } else {
            h.set_base(recorded[first->set].base);
            State s = first->state;
            for (std::size_t k = s.pokes.size(); k-- > 0 && budget-- > 0;) {
                State t = s;
                t.pokes.erase(t.pokes.begin() + static_cast<std::ptrdiff_t>(k));
                Verdict v = h.check(t, false);
                if (!v.abi.empty()) {
                    s = std::move(t);
                    minimised = s;
                    minimised_verdict = std::move(v);
                }
            }
        }
        if (!minimised) {
            minimised = first->state;
            minimised_verdict = first->verdict;
        }
    }

    // ---- text report ----
    const Status status = classify(total, failed, returned, spec.min_returned);
    const bool abi_pass = failed == 0;
    const double returned_share = total ? static_cast<double>(returned) / static_cast<double>(total) : 0.0;
    std::printf("states: %llu random (seed %llu)", static_cast<unsigned long long>(random_states),
                static_cast<unsigned long long>(options.seed));
    std::uint64_t recorded_total = 0;
    for (const auto& set : recorded) recorded_total += set.states.size();
    std::printf(", %llu recorded\n", static_cast<unsigned long long>(recorded_total));
    std::printf("ABI (gate): %s %llu/%llu\n", abi_pass ? "PASS" : "FAIL",
                static_cast<unsigned long long>(total - failed), static_cast<unsigned long long>(total));
    std::printf("returned normally (gate): %llu/%llu (%.1f%%, required %.1f%%)\n",
                static_cast<unsigned long long>(returned), static_cast<unsigned long long>(total),
                100.0 * returned_share, 100.0 * spec.min_returned);
    for (const Tally& t : tallies) {
        std::printf("  %-16s %llu/%llu%s\n", t.name.c_str(), static_cast<unsigned long long>(t.states - t.failed),
                    static_cast<unsigned long long>(t.states),
                    t.stopped ? (" (" + std::to_string(t.stopped) + " stopped the same way in both)").c_str() : "");
    }
    for (const auto& [key, count] : abi_counts) {
        std::printf("  differs: %-12s in %llu states\n", key.c_str(), static_cast<unsigned long long>(count));
    }
    std::printf("full state: identical in %llu/%llu\n", static_cast<unsigned long long>(full_identical),
                static_cast<unsigned long long>(total));
    for (const auto& [key, count] : full_counts) {
        std::printf("  differs: %-12s in %llu states\n", key.c_str(), static_cast<unsigned long long>(count));
    }
    for (const auto& [why, count] : stops) {
        std::printf("both stopped the same way in %llu states: %s\n", static_cast<unsigned long long>(count), why.c_str());
    }
    const std::vector<std::uint64_t>& hits = h.hits();
    std::size_t covered = 0;
    std::vector<std::uint32_t> uncovered;
    for (std::size_t i = 0; i < hits.size(); ++i) {
        if (hits[i]) {
            ++covered;
        } else {
            uncovered.push_back(e.block_addrs[i]);
        }
    }
    const double percent = hits.empty() ? 0.0 : 100.0 * static_cast<double>(covered) / static_cast<double>(hits.size());
    if (hits.empty()) {
        std::printf("coverage: unknown (the program was generated without block counters)\n");
    } else {
        std::printf("coverage: %zu/%zu blocks of the translation (%.1f%%)", covered, hits.size(), percent);
        std::string list;
        for (std::size_t i = 0; i < uncovered.size() && i < 16; ++i) list += (i ? ", " : "") + hex32(uncovered[i]);
        if (uncovered.size() > 16) list += ", ...";
        std::printf("%s\n", uncovered.empty() ? "" : ("; not run: " + list).c_str());
    }
    std::string masks = "dead stack " + std::to_string(spec.dead_stack) + " bytes below sp";
    for (const auto& [lo, hi] : spec.ignore_memory) masks += "; ignored " + hex32(lo) + "-" + hex32(hi - 1);
    std::printf("masks: %s; tick limit %llu\n", masks.c_str(), static_cast<unsigned long long>(tick_limit));
    if (first) {
        std::printf("first failing state: #%llu (%s)\n", static_cast<unsigned long long>(first->number),
                    first->random ? ("random, shape " + spec.shapes[first->shape].name + ", index " +
                                     std::to_string(first->index)).c_str()
                                  : ("recorded, " + recorded[first->set].file + " #" + std::to_string(first->index)).c_str());
        std::printf("  minimised input: %s\n", describe_state(*minimised).c_str());
        std::printf("  differences (ABI):\n");
        for (const Diff& d : minimised_verdict->abi) std::printf("    %s\n", d.text.c_str());
        if (!minimised_verdict->full.empty()) {
            std::printf("  other differences:\n");
            for (const Diff& d : minimised_verdict->full) std::printf("    %s\n", d.text.c_str());
        }
    }

    // ---- JSON report ----
    if (options.json) {
        Json j = Json::object();
        j["function"] = std::string(e.name);
        j["addr"] = hex32(e.addr);
        j["thumb"] = e.thumb != 0;
        j["impl"] = impl_name;
        j["configured_impl"] = std::string(e.impl);
        j["verified_in_config"] = e.verified != 0;
        j["pass"] = status == Status::Pass;
        j["status"] = std::string(status_name(status));
        Json states = Json::object();
        states["total"] = static_cast<unsigned long long>(total);
        states["random"] = static_cast<unsigned long long>(random_states);
        states["recorded"] = static_cast<unsigned long long>(recorded_total);
        states["seed"] = static_cast<unsigned long long>(options.seed);
        states["passed"] = static_cast<unsigned long long>(total - failed);
        states["failed"] = static_cast<unsigned long long>(failed);
        states["full_identical"] = static_cast<unsigned long long>(full_identical);
        states["returned"] = static_cast<unsigned long long>(returned);
        states["returned_share"] = returned_share;
        Json stop_json = Json::object();
        for (const auto& [why, count] : stops) stop_json[why] = static_cast<unsigned long long>(count);
        states["same_stop"] = std::move(stop_json);
        j["states"] = std::move(states);
        Json shapes = Json::array();
        for (const Tally& t : tallies) {
            Json s = Json::object();
            s["name"] = t.name;
            s["states"] = static_cast<unsigned long long>(t.states);
            s["failed"] = static_cast<unsigned long long>(t.failed);
            s["stopped"] = static_cast<unsigned long long>(t.stopped);
            shapes.push_back(std::move(s));
        }
        j["shapes"] = std::move(shapes);
        Json abi = Json::object();
        for (const auto& [key, count] : abi_counts) abi[key] = static_cast<unsigned long long>(count);
        j["abi_differences"] = std::move(abi);
        Json full = Json::object();
        for (const auto& [key, count] : full_counts) full[key] = static_cast<unsigned long long>(count);
        j["full_differences"] = std::move(full);
        Json cov = Json::object();
        cov["blocks"] = static_cast<unsigned long long>(hits.size());
        cov["covered"] = static_cast<unsigned long long>(covered);
        cov["percent"] = percent;
        Json unc = Json::array();
        for (const std::uint32_t a : uncovered) unc.push_back(hex32(a));
        cov["uncovered"] = std::move(unc);
        Json hit_list = Json::array();
        for (std::size_t i = 0; i < hits.size(); ++i) {
            Json hj = Json::object();
            hj["addr"] = hex32(e.block_addrs[i]);
            hj["runs"] = static_cast<unsigned long long>(hits[i]);
            hit_list.push_back(std::move(hj));
        }
        cov["hits"] = std::move(hit_list);
        j["coverage"] = std::move(cov);
        Json mask = Json::object();
        mask["dead_stack"] = static_cast<unsigned long long>(spec.dead_stack);
        Json ign = Json::array();
        for (const auto& [lo, hi] : spec.ignore_memory) {
            Json pr = Json::array();
            pr.push_back(hex32(lo));
            pr.push_back(hex32(hi));
            ign.push_back(std::move(pr));
        }
        mask["ignore_memory"] = std::move(ign);
        mask["tick_limit"] = static_cast<unsigned long long>(tick_limit);
        mask["min_returned"] = spec.min_returned;
        j["masks"] = std::move(mask);
        if (first) {
            Json f = Json::object();
            f["state"] = static_cast<unsigned long long>(first->number);
            f["source"] = first->random ? "random" : "recorded";
            if (first->random) {
                f["shape"] = spec.shapes[first->shape].name;
            } else {
                f["file"] = recorded[first->set].file;
            }
            f["index"] = static_cast<unsigned long long>(first->index);
            f["input"] = state_json(first->state);
            f["differences"] = diffs_json(first->verdict.abi);
            f["full_differences"] = diffs_json(first->verdict.full);
            f["minimised_input"] = state_json(*minimised);
            f["minimised_differences"] = diffs_json(minimised_verdict->abi);
            f["minimised_full_differences"] = diffs_json(minimised_verdict->full);
            j["first_failure"] = std::move(f);
        } else {
            j["first_failure"] = nullptr;
        }
        const std::string text = j.dump() + "\n";
        if (options.json->string() == "-") {
            std::fputs(text.c_str(), stdout);
        } else {
            write_text_file(*options.json, text);
        }
    }
    std::printf("result: %s\n", status == Status::Pass   ? "PASS"
                                : status == Status::Fail ? "FAIL"
                                                         : "INSUFFICIENT (too few states returned normally; widen the "
                                                           "tick limit or bound the spec's inputs)");
    std::fflush(stdout);
    return exit_code(status);
}

}  // namespace anygba::difftest
