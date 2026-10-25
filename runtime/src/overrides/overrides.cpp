#include "overrides/overrides.h"

#include "anygba/override.hpp"
#include "anygba/system.hpp"

#include <cstdio>
#include <map>
#include <set>

namespace anygba::overrides {

namespace {

// Function-local statics: ANYGBA_OVERRIDE registers during static initialisation, in any order.
std::map<std::string, guest::Impl, std::less<>>& registry() {
    static std::map<std::string, guest::Impl, std::less<>> impls;
    return impls;
}

std::set<std::string>& duplicates() {
    static std::set<std::string> names;
    return names;
}

const anygba_game* g_game = nullptr;
std::vector<guest::Impl> g_impls;  // by override index
std::function<void(const std::string&)> g_osd;

const anygba_override_entry* entry_at(std::uint32_t addr) {
    if (!g_game) return nullptr;
    for (std::uint32_t i = 0; i < g_game->override_count; ++i) {
        if (g_game->overrides[i].addr == addr) return &g_game->overrides[i];
    }
    return nullptr;
}

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

}  // namespace

std::optional<Mode> parse_mode(std::string_view text) {
    if (text == "on") return Mode::On;
    if (text == "off") return Mode::Off;
    if (text == "all") return Mode::All;
    return std::nullopt;
}

std::vector<Active> activate(const anygba_game& game, Mode mode) {
    std::vector<Active> result;
    if (game.override_count == 0) {
        return result;
    }
    if (!game.overrides || !game.override_on) {
        throw Error("the game's override table is incomplete");
    }
    std::vector<guest::Impl> impls(game.override_count);
    std::string missing;
    for (std::uint32_t i = 0; i < game.override_count; ++i) {
        const anygba_override_entry& e = game.overrides[i];
        if (duplicates().contains(e.impl)) {
            throw Error(std::string("override ") + e.name + " at " + hex(e.addr) + ": implementation '" + e.impl +
                        "' is registered twice (two ANYGBA_OVERRIDE with that name)");
        }
        const auto it = registry().find(std::string_view(e.impl));
        if (it == registry().end()) {
            missing += std::string(missing.empty() ? "" : "; ") + "override " + e.name + " at " + hex(e.addr) +
                       ": no implementation '" + e.impl + "'";
            continue;
        }
        impls[i] = it->second;
    }
    if (!missing.empty()) {
        std::string known;
        for (const auto& [name, impl] : registry()) known += (known.empty() ? "" : ", ") + name;
        throw Error(missing + " is compiled into this program (registered: " + (known.empty() ? "none" : known) + ")");
    }
    g_game = &game;
    g_impls = std::move(impls);
    for (std::uint32_t i = 0; i < game.override_count; ++i) {
        const bool on = mode == Mode::All || (mode == Mode::On && game.overrides[i].enabled);
        game.override_on[i] = on ? 1 : 0;
        result.push_back({&game.overrides[i], on});
    }
    return result;
}

std::string describe(const Active& a) {
    const anygba_override_entry& e = *a.entry;
    const std::string where = hex(e.addr);
    std::string text = (e.name == where ? where : std::string(e.name) + " at " + where) + (e.thumb ? " thumb" : " arm") +
                       " -> " + e.impl;
    if (e.engine && *e.engine) text += std::string(" [") + e.engine + "]";
    text += a.on ? ": on" : e.enabled ? ": off" : ": off (disabled in the config)";
    return text;
}

void set_osd_sink(std::function<void(const std::string&)> sink) { g_osd = std::move(sink); }

}  // namespace anygba::overrides

namespace anygba::guest {

bool register_impl(const char* name, Impl impl) noexcept {
    try {
        if (!overrides::registry().emplace(name, impl).second) {
            overrides::duplicates().insert(name);
        }
        return true;
    } catch (...) {
        return false;  // out of memory during static initialisation; activate() reports it missing
    }
}

bool Guest::call(std::uint32_t target, std::uint32_t return_addr) {
    const std::uint32_t expected = return_addr & ~1u;
    c_->r[14] = return_addr;
    c_->t = target & 1u;
    c_->r[15] = target & (c_->t ? ~1u : ~3u);
    if (c_->ret_depth >= ANYGBA_RET_STACK) anygba_unwind(c_);
    c_->ret_stack[c_->ret_depth++] = expected;
    const int outer = anygba_resume(c_, expected);
    c_->ret_depth--;
    if (outer) {
        returned_ = true;  // r[15] already names where an outer call site continues
        return false;
    }
    return true;
}

void Guest::run_original() {
    const anygba_override_entry* e = overrides::entry_at(entry_);
    if (!e) {
        system_of(c_).fail("override at " + overrides::hex(entry_) + ": no original translation to run");
    }
    e->original(c_);
    returned_ = true;
}

void Guest::osd(std::string_view text) {
    if (overrides::g_osd) overrides::g_osd(std::string(text));
}

}  // namespace anygba::guest

extern "C" void anygba_override_run(anygba_cpu* c, std::uint32_t index) {
    using namespace anygba;
    if (!overrides::g_game || index >= overrides::g_impls.size()) {
        system_of(c).fail("override " + std::to_string(index) + " ran before the overrides were activated");
    }
    const anygba_override_entry& e = overrides::g_game->overrides[index];
    guest::Guest g(c, e.addr);
    overrides::g_impls[index](g);
    if (!g.returned()) {
        system_of(c).fail(std::string("override ") + e.name + " (" + e.impl +
                          ") finished without ret(), ret_to() or run_original()");
    }
}
