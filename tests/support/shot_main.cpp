// anygba-shot: runs a ROM headless in the interpreter and writes what tests/oracle/mgba-oracle.py
// writes for mGBA (frame hashes, PNG screenshots), so the two can be compared directly.
#include "support/corpus.h"
#include "support/headless_run.h"
#include "support/png.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr const char* kUsage =
    "usage: anygba-shot ROM --frames N [--shot F:out.png] [--hash F] [--keys KEY:A-B] [--dump REGION:out.bin]\n"
    "  REGION is ewram, iwram, io, palette, vram or oam, written after the last frame\n";

void write_region(const anygba::Memory& m, const std::string& region, const std::string& path) {
    const std::vector<std::uint8_t>* data = region == "ewram"     ? &m.ewram
                                            : region == "iwram"   ? &m.iwram
                                            : region == "io"      ? &m.io
                                            : region == "palette" ? &m.palette
                                            : region == "vram"    ? &m.vram
                                            : region == "oam"     ? &m.oam
                                                                  : nullptr;
    if (!data) {
        throw std::invalid_argument("unknown memory region " + region);
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot write " + path);
    }
    f.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string rom;
        std::uint64_t frames = 0;
        std::map<std::uint64_t, std::vector<std::string>> shots;
        std::set<std::uint64_t> hashes;
        std::vector<anygba::test::KeyHold> keys;
        std::vector<std::pair<std::string, std::string>> dumps;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            const bool has_value = i + 1 < argc;
            if (arg == "--frames" && has_value) {
                frames = std::strtoull(argv[++i], nullptr, 10);
            } else if (arg == "--shot" && has_value) {
                const std::string spec = argv[++i];
                const auto colon = spec.find(':');
                if (colon == std::string::npos) {
                    std::fputs(kUsage, stderr);
                    return 2;
                }
                shots[std::strtoull(spec.substr(0, colon).c_str(), nullptr, 10)].push_back(spec.substr(colon + 1));
            } else if (arg == "--hash" && has_value) {
                hashes.insert(std::strtoull(argv[++i], nullptr, 10));
            } else if (arg == "--dump" && has_value) {
                const std::string spec = argv[++i];
                const auto colon = spec.find(':');
                if (colon == std::string::npos) {
                    std::fputs(kUsage, stderr);
                    return 2;
                }
                dumps.emplace_back(spec.substr(0, colon), spec.substr(colon + 1));
            } else if (arg == "--keys" && has_value) {
                keys.push_back(anygba::test::parse_key_hold(argv[++i]));
            } else if (rom.empty() && !arg.starts_with("-")) {
                rom = arg;
            } else {
                std::fputs(kUsage, stderr);
                return 2;
            }
        }
        if (rom.empty() || frames == 0) {
            std::fputs(kUsage, stderr);
            return 2;
        }
        anygba::test::run_headless(anygba::test::read_file(rom), frames, keys, [&](std::uint64_t f, anygba::System& s) {
            if (const auto it = shots.find(f); it != shots.end()) {
                for (const std::string& path : it->second) {
                    anygba::test::write_png(path, s.framebuffer(), anygba::kScreenWidth, anygba::kScreenHeight);
                }
            }
            if (hashes.contains(f)) {
                std::printf("frame %llu sha256 %s\n", static_cast<unsigned long long>(f),
                            anygba::test::frame_hash(s.framebuffer()).c_str());
            }
            if (f == frames) {
                for (const auto& [region, path] : dumps) {
                    write_region(s.memory(), region, path);
                }
            }
        });
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
