#include "pipeline.h"

#include "anygba/sha256.h"
#include "codegen/c_emitter.h"
#include "config/game_config.h"
#include "decode/disasm.h"
#include "io/miss_log.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <ostream>
#include <stdexcept>

namespace anygba::recomp {

namespace {

std::vector<std::uint8_t> read_rom(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open ROM: " + path.string());
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (bytes.size() < 0xC0 || bytes.size() > analysis::kRomMaxSize) {
        throw std::runtime_error("not a GBA ROM (size " + std::to_string(bytes.size()) + " bytes): " + path.string());
    }
    return bytes;
}

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

// Overrides from the config and the modules, with symbols looked up in the [functions] tables of
// the config and the symbols files. Adds the modules' sources to `analysis`.
void resolve_overrides(const Options& options, const config::GameConfig* game, const std::string& game_code,
                       Analysis& analysis, analysis::DiscoveryOptions& discovery) {
    std::vector<config::Override> wanted;
    if (game) wanted = game->overrides;
    for (const auto& dir : options.override_modules) {
        config::OverrideModule module = config::load_override_module(dir);
        wanted.insert(wanted.end(), module.overrides.begin(), module.overrides.end());
        analysis.sources.insert(analysis.sources.end(), module.sources.begin(), module.sources.end());
    }
    // An engine module may hold overrides that replay one game's machine code; other games skip them.
    std::erase_if(wanted, [&](const config::Override& o) {
        return !o.games.empty() && std::find(o.games.begin(), o.games.end(), game_code) == o.games.end();
    });
    if (wanted.empty()) return;
    if (analysis.sources.empty()) {
        throw config::ConfigError(wanted.front().where +
                                  ": [[override]] needs sources: an override module (--overrides DIR) or game.sources");
    }

    config::FunctionTable functions;
    std::string tables;  // where symbols were looked up, for the error message
    const auto merge = [&](const config::FunctionTable& table, const std::string& from) {
        tables += (tables.empty() ? "" : ", ") + from;
        for (const auto& [name, fn] : table) {
            const auto [it, added] = functions.emplace(name, fn);
            if (!added && (it->second.addr != fn.addr || it->second.mode != fn.mode)) {
                throw config::ConfigError(from + ": functions." + name + " differs from an earlier [functions] table");
            }
        }
    };
    if (std::any_of(wanted.begin(), wanted.end(), [](const config::Override& o) { return !o.addr; })) {
        if (game && options.config) merge(game->functions, options.config->string());
        for (const auto& file : options.symbol_files) merge(config::load_function_symbols(file), file.string());
    }

    std::map<std::uint32_t, std::string> seen;  // addr -> where
    for (const auto& o : wanted) {
        codegen::OverrideInfo info;
        info.impl = o.impl;
        info.engine = o.engine;
        info.enabled = o.enabled;
        info.verified = o.verified;
        if (o.addr) {
            info.addr = *o.addr;
            info.thumb = o.mode == analysis::Mode::Thumb;
            info.name = hex(info.addr);
        } else {
            const auto it = functions.find(o.symbol);
            if (it == functions.end()) {
                throw config::ConfigError(o.where + ": override.symbol '" + o.symbol + "' is not in any [functions] table" +
                                          (tables.empty() ? std::string(" (pass --symbols FILE)") : " (" + tables + ")"));
            }
            info.addr = it->second.addr;
            info.thumb = it->second.mode == analysis::Mode::Thumb;
            info.name = o.symbol;
        }
        if ((info.addr & (info.thumb ? 1u : 3u)) != 0) {
            throw config::ConfigError(o.where + ": " + info.name + " is at the misaligned address " + hex(info.addr) +
                                      " (give the address without the Thumb bit)");
        }
        if (const auto [it, added] = seen.emplace(info.addr, o.where); !added) {
            throw config::ConfigError(o.where + ": a second override for " + hex(info.addr) + " (first at " + it->second + ")");
        }
        if (analysis.replacements.contains(info.addr)) {
            throw config::ConfigError(o.where + ": " + hex(info.addr) + " already has a [[replace]] entry");
        }
        discovery.entries.push_back({info.addr, info.thumb ? analysis::Mode::Thumb : analysis::Mode::Arm,
                                     analysis::EntrySource::Config});
        analysis.overrides.push_back(std::move(info));
    }
    std::sort(analysis.overrides.begin(), analysis.overrides.end(),
              [](const codegen::OverrideInfo& a, const codegen::OverrideInfo& b) { return a.addr < b.addr; });
}

}  // namespace

Analysis analyze(const Options& options) {
    Analysis result;
    std::vector<std::uint8_t> rom = read_rom(options.rom);
    result.rom_sha256 = sha256_hex(rom);
    result.rom_size = static_cast<std::uint32_t>(rom.size());
    for (std::size_t i = 0xA0; i < 0xAC && rom[i] != 0; ++i) {
        result.title += static_cast<char>(rom[i]);
    }
    result.image = std::make_unique<analysis::Image>(std::move(rom));
    analysis::DiscoveryOptions discovery_options;
    std::optional<config::GameConfig> game_config;
    if (options.config) {
        game_config = config::load_game_config(*options.config);
        const config::GameConfig& game = *game_config;
        if (!game.title.empty()) {
            result.title = game.title;
        }
        result.save_type = game.save_type;
        result.rtc = game.rtc;
        result.video = game.video;
        result.sources = game.sources;
        for (const auto& rep : game.replacements) {
            if (!result.replacements.emplace(rep.addr, rep.function).second) {
                throw config::ConfigError("two [[replace]] entries for the same address");
            }
        }
        discovery_options = config::apply_game_config(game, *result.image);
    }
    const std::span<const std::uint8_t> bytes = result.image->rom();
    const std::string game_code = bytes.size() >= 0xB0 ? std::string(bytes.begin() + 0xAC, bytes.begin() + 0xB0) : "";
    resolve_overrides(options, game_config ? &*game_config : nullptr, game_code, result, discovery_options);
    for (const auto& e : discovery_options.entries) {
        for (const auto& r : discovery_options.skip) {
            if (e.source == analysis::EntrySource::Config && r.contains(e.addr)) {
                throw config::ConfigError("the entry point " + hex(e.addr) + " (from [entry], [[ram_region]], [[replace]] or "
                                          "[[override]]) lies in [[skip]] " + hex(r.begin) + "-" + hex(r.end) +
                                          ": skipped bytes are never translated; shrink the skip range");
            }
        }
    }
    discovery_options.entries.insert(discovery_options.entries.begin(),
                                     {analysis::kRomBase, analysis::Mode::Arm, analysis::EntrySource::RomEntry});
    for (const auto& path : options.miss_logs) {
        io::apply_miss_log(io::read_miss_log(path), *result.image, discovery_options.entries);
    }
    result.discovery = analysis::discover(*result.image, discovery_options);
    return result;
}

std::size_t generate(const Options& options, const Analysis& analysis) {
    codegen::ProjectInfo info;
    info.title = analysis.title;
    info.rom_sha256 = analysis.rom_sha256;
    info.rom_size = analysis.rom_size;
    info.shards = options.shards;
    info.save_type = analysis.save_type;
    info.rtc = analysis.rtc;
    info.video = analysis.video;
    info.replacements = analysis.replacements;
    info.sources = analysis.sources;
    info.overrides = analysis.overrides;
    codegen::write_project(options.output_dir, *analysis.image, analysis.discovery, info);
    return analysis.discovery.functions.size();
}

void write_listing(std::ostream& out, const Analysis& analysis) {
    char buf[96];
    for (const auto& [entry, fn] : analysis.discovery.functions) {
        std::snprintf(buf, sizeof buf, "\nfunction 0x%08x %s (%s)%s\n", entry,
                      fn.mode == analysis::Mode::Arm ? "arm" : "thumb", analysis::to_string(fn.source),
                      fn.hit_undefined ? " [hit undefined]" : fn.noreturn ? " [noreturn]" : "");
        out << buf;
        for (const auto& [addr, in] : fn.insns) {
            std::snprintf(buf, sizeof buf, "%s%08x:  ", fn.labels.contains(addr) ? "L " : "  ", addr);
            out << buf << decode::disassemble(in) << '\n';
        }
    }
    for (const auto& note : analysis.discovery.notes) {
        out << "note: " << note << '\n';
    }
    std::snprintf(buf, sizeof buf, "\n%zu functions\n", analysis.discovery.functions.size());
    out << buf;
}

}  // namespace anygba::recomp
