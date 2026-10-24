#include "pipeline.h"

#include "anygba/sha256.h"
#include "codegen/c_emitter.h"
#include "config/game_config.h"
#include "decode/disasm.h"
#include "io/miss_log.h"

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
    if (options.config) {
        const config::GameConfig game = config::load_game_config(*options.config);
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
