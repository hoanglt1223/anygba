#include "anygba/sha256.h"
#include "anygba/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

extern "C" const anygba_game anygba_game_info;

namespace {

struct Args {
    std::string rom;
    bool headless = false;
    std::optional<std::uint64_t> frames;
    bool exit_on_idle = false;
    anygba::Options runtime;
};

const char* const kUsage =
    "usage: game <rom.gba> [--headless] [--frames N] [--exit-on-idle] [--miss-log FILE | --no-miss-log]\n"
    "\n"
    "  --headless       no window and no audio\n"
    "  --frames N       stop after N frames\n"
    "  --exit-on-idle   stop at the first branch-to-self and print the registers\n"
    "  --miss-log FILE  where to append addresses the recompiled code missed (default anygba-miss.log)\n";

std::optional<Args> parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--headless") {
            a.headless = true;
        } else if (arg == "--exit-on-idle") {
            a.exit_on_idle = true;
        } else if (arg == "--no-miss-log") {
            a.runtime.log_misses = false;
        } else if (arg == "--frames" && has_value) {
            char* end = nullptr;
            const unsigned long long n = std::strtoull(argv[++i], &end, 10);
            if (*end != '\0' || n == 0) {
                return std::nullopt;
            }
            a.frames = n;
        } else if (arg == "--miss-log" && has_value) {
            a.runtime.miss_log = argv[++i];
        } else if (!arg.starts_with("-") && a.rom.empty()) {
            a.rom = arg;
        } else {
            return std::nullopt;
        }
    }
    if (a.rom.empty()) {
        return std::nullopt;
    }
    return a;
}

void print_registers(const anygba_cpu& c) {
    for (int r = 0; r < 16; ++r) {
        std::printf("r%d=%08x%s", r, c.r[r], r % 4 == 3 ? "\n" : " ");
    }
    std::printf("cpsr=%08x\n", ag_cpsr(&c));
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = parse(argc, argv);
    if (!args) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    std::ifstream file(args->rom, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot open ROM: %s\n", args->rom.c_str());
        return 1;
    }
    std::vector<std::uint8_t> rom{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (anygba::sha256_hex(rom) != anygba_game_info.rom_sha256) {
        std::fprintf(stderr, "%s is not the ROM this program was generated from (expected SHA-256 %s)\n",
                     args->rom.c_str(), anygba_game_info.rom_sha256);
        return 1;
    }
    if (!args->headless) {
        std::fputs("windowed mode is not available yet; pass --headless\n", stderr);
        return 2;
    }

    std::unique_ptr<anygba::System> system;
    try {
        system = std::make_unique<anygba::System>(std::move(rom), &anygba_game_info, args->runtime);
        anygba::System& sys = *system;
        bool idle = false;
        if (args->frames) {
            const std::uint64_t limit = *args->frames;
            sys.on_vblank = [limit](anygba::System& s) {
                if (s.frames() >= limit) {
                    s.stop();
                }
            };
        }
        if (args->exit_on_idle) {
            sys.on_idle_loop = [&idle](anygba::System& s) {
                idle = true;
                s.stop();
            };
        }
        sys.run();
        if (idle) {
            std::printf("idle loop at 0x%08x after %llu frames\n", sys.cpu().r[15],
                        static_cast<unsigned long long>(sys.frames()));
            print_registers(sys.cpu());
        } else {
            std::printf("stopped after %llu frames\n", static_cast<unsigned long long>(sys.frames()));
            print_registers(sys.cpu());
        }
        if (!sys.misses().empty()) {
            std::printf("%zu addresses ran in the interpreter (see %s)\n", sys.misses().size(),
                        args->runtime.miss_log.string().c_str());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        if (system) {
            print_registers(system->cpu());
        }
        return 1;
    }
}
