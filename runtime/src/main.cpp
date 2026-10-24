#include "anygba/sha256.h"
#include "anygba/system.hpp"
#include "anygba/embedded_rom.h"
#include "input_map.h"
#include "save/save.h"

#include <anyport/platform/audio.h>
#include <anyport/platform/context.h>
#include <anyport/platform/input.h>
#include <anyport/platform/timing.h>
#include <anyport/platform/window.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern "C" const anygba_game anygba_game_info;

namespace {

namespace platform = anyport::platform;
namespace fs = std::filesystem;

constexpr int kSampleRate = anygba::System::kAudioRate;

struct Args {
    std::string rom;  // empty: the ROM embedded by `anygba build`
    bool headless = false;
    bool pace = true;  // headless runs unpaced unless --pace
    bool pace_given = false;
    int scale = 3;
    std::optional<std::uint64_t> frames;
    std::optional<double> seconds;
    bool exit_on_idle = false;
    bool frame_hash = false;
    std::optional<fs::path> save_dir;
    bool no_save = false;
    std::optional<fs::path> input_file;
    bool write_input = false;
    anygba::Options runtime;
};

const char* const kUsage =
    "usage: game [rom.gba] [options]\n"
    "\n"
    "  rom.gba             the ROM this program was generated from; omit it when the ROM is embedded\n"
    "  --headless          no window and no audio (SDL dummy drivers); runs unpaced unless --pace\n"
    "  --pace              pace to real time (default with a window)\n"
    "  --fast              do not pace\n"
    "  --scale N           initial window size as a multiple of 240x160 (default 3)\n"
    "  --frames N          stop after N frames\n"
    "  --seconds S         stop after S seconds of wall-clock time\n"
    "  --exit-on-idle      stop at the first branch-to-self and print the registers\n"
    "  --dump-frame-hash   print the SHA-256 of the last frame (RGB555, little-endian) on exit\n"
    "  --save-dir DIR      where the save file lives (default: the per-user data folder)\n"
    "  --save-type T       none, sram, flash64, flash128, eeprom, eeprom512 or eeprom8k (default: config, else detect)\n"
    "  --no-save           do not read or write a save file\n"
    "  --input FILE        input mapping (default: anygba-input.ini next to the program, else in the data folder)\n"
    "  --write-input FILE  write the current input mapping to FILE and exit\n"
    "  --miss-log FILE     where to append addresses the recompiled code missed (default anygba-miss.log)\n"
    "  --no-miss-log       do not write the miss log\n";

std::optional<Args> parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        char* end = nullptr;
        if (arg == "--headless") {
            a.headless = true;
        } else if (arg == "--pace") {
            a.pace = true;
            a.pace_given = true;
        } else if (arg == "--fast") {
            a.pace = false;
            a.pace_given = true;
        } else if (arg == "--exit-on-idle") {
            a.exit_on_idle = true;
        } else if (arg == "--dump-frame-hash") {
            a.frame_hash = true;
        } else if (arg == "--no-miss-log") {
            a.runtime.log_misses = false;
        } else if (arg == "--no-save") {
            a.no_save = true;
        } else if (arg == "--frames" && has_value) {
            const unsigned long long n = std::strtoull(argv[++i], &end, 10);
            if (*end != '\0' || n == 0) return std::nullopt;
            a.frames = n;
        } else if (arg == "--seconds" && has_value) {
            const double s = std::strtod(argv[++i], &end);
            if (*end != '\0' || !(s > 0)) return std::nullopt;
            a.seconds = s;
        } else if (arg == "--scale" && has_value) {
            const long n = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || n < 1 || n > 16) return std::nullopt;
            a.scale = static_cast<int>(n);
        } else if (arg == "--miss-log" && has_value) {
            a.runtime.miss_log = argv[++i];
        } else if (arg == "--save-dir" && has_value) {
            a.save_dir = fs::path(argv[++i]);
        } else if (arg == "--save-type" && has_value) {
            a.runtime.save_type = argv[++i];
            if (!anygba::save::parse(a.runtime.save_type)) return std::nullopt;
        } else if (arg == "--input" && has_value) {
            a.input_file = fs::path(argv[++i]);
        } else if (arg == "--write-input" && has_value) {
            a.input_file = fs::path(argv[++i]);
            a.write_input = true;
        } else if (!arg.starts_with("-") && a.rom.empty()) {
            a.rom = arg;
        } else {
            return std::nullopt;
        }
    }
    if (a.headless && !a.pace_given) {
        a.pace = false;
    }
    return a;
}

void print_registers(const anygba_cpu& c) {
    for (int r = 0; r < 16; ++r) {
        std::printf("r%d=%08x%s", r, c.r[r], r % 4 == 3 ? "\n" : " ");
    }
    std::printf("cpsr=%08x\n", ag_cpsr(&c));
}

fs::path utf8_path(const char* s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s))); }

// Per-user data folder (created by SDL), e.g. %APPDATA%/anyport/anygba/ on Windows.
std::optional<fs::path> data_dir() {
    char* path = SDL_GetPrefPath("anyport", "anygba");
    if (!path) {
        return std::nullopt;
    }
    fs::path result = utf8_path(path);
    SDL_free(path);
    return result;
}

std::optional<fs::path> program_dir() {
    const char* base = SDL_GetBasePath();  // owned by SDL
    return base ? std::optional<fs::path>(utf8_path(base)) : std::nullopt;
}

std::string save_file_name() {
    std::string name;
    for (const char* p = anygba_game_info.title; *p; ++p) {
        const char ch = *p;
        const bool safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                          ch == '_';
        name += safe ? ch : '_';
    }
    // The hash prefix keeps two builds that share a title (or have none) apart.
    return (name.empty() ? "game" : name) + "-" + std::string(anygba_game_info.rom_sha256).substr(0, 8) + ".sav";
}

anygba::input::Mapping load_input(const Args& args) {
    if (args.input_file && !args.write_input) {
        return anygba::input::load_mapping(*args.input_file);
    }
    for (const auto& dir : {program_dir(), data_dir()}) {
        if (dir && fs::exists(*dir / "anygba-input.ini")) {
            return anygba::input::load_mapping(*dir / "anygba-input.ini");
        }
    }
    return anygba::input::default_mapping();
}

// Host side of a running game: window, audio, input, pacing, and save writes, all driven
// from VBlank.
class Frontend {
public:
    Frontend(const Args& args, anygba::System& sys, anygba::input::Mapping mapping, anygba::save::SaveFile* save)
        : args_(args), sys_(sys), mapping_(std::move(mapping)), save_(save) {
        if (!args.headless) {
            context_ = std::make_unique<platform::Context>(platform::ContextOptions{false});
            platform::WindowConfig window;
            window.title = anygba_game_info.title;
            window.width = anygba::kScreenWidth;
            window.height = anygba::kScreenHeight;
            window.scale = args.scale;
            window.vsync = false;  // pacing follows the audio clock, not the display
            window_ = std::make_unique<platform::Window>(window);
            audio_ = std::make_unique<platform::AudioStream>(platform::AudioConfig{kSampleRate, 2});
            input_ = std::make_unique<platform::Input>();
            input_->clear_key_bindings();  // keys are read by name through the mapping
        }
        if (args.pace) {
            pacer_ = std::make_unique<platform::FramePacer>(anygba::kFramesPerSecond);
        }
        start_ns_ = platform::now_ns();
    }

    void on_vblank() {
        if (args_.frames && sys_.frames() >= *args_.frames) {
            sys_.stop();
        }
        if (args_.seconds && static_cast<double>(platform::now_ns() - start_ns_) / 1e9 >= *args_.seconds) {
            sys_.stop();
        }
        if (save_) {
            if (const auto error = save_->on_frame()) {
                std::fprintf(stderr, "warning: %s\n", error->c_str());
            }
        }
        bool fast_forward = !args_.pace;
        if (input_) {
            if (!platform::pump_events(*input_)) {
                sys_.stop();
            }
            sys_.set_keys(anygba::input::gba_keys(mapping_, *input_));
            fast_forward = fast_forward || anygba::input::fast_forward_held(mapping_, *input_);
        }
        if (window_) {
            window_->present(sys_.framebuffer());
        }
        sound_.clear();
        sys_.drain_audio(sound_);
        if (audio_) {
            feed_audio_and_wait(fast_forward);
        } else if (pacer_ && !fast_forward) {
            pacer_->wait();
        }
    }

private:
    // Audio-led pacing: the device consumes 32768 frames per emulated second (SDL resamples
    // to the device rate), so keeping the queue near two video frames ties emulation speed to
    // the sound card's clock and the queue can neither run dry nor grow.
    void feed_audio_and_wait(bool fast_forward) {
        const std::size_t target = 2 * kSampleRate / 60;
        if (fast_forward && audio_->queued_frames() > target) {
            return;  // drop audio while fast-forwarding
        }
        audio_->push(sound_);
        while (!fast_forward && audio_->queued_frames() > target) {
            platform::sleep_ns(1'000'000);
        }
    }

    const Args& args_;
    anygba::System& sys_;
    anygba::input::Mapping mapping_;
    anygba::save::SaveFile* save_;
    std::unique_ptr<platform::Context> context_;
    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<platform::AudioStream> audio_;
    std::unique_ptr<platform::Input> input_;
    std::unique_ptr<platform::FramePacer> pacer_;
    std::vector<std::int16_t> sound_;
    std::uint64_t start_ns_ = 0;
};

std::vector<std::uint8_t> read_rom(const Args& args) {
    if (args.rom.empty()) {
        auto rom = anygba::read_embedded_rom();
        if (!rom) {
            throw std::runtime_error("no ROM given and none embedded in this program");
        }
        return std::move(*rom);
    }
    std::ifstream file(args.rom, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open ROM: " + args.rom);
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = parse(argc, argv);
    if (!args) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    std::unique_ptr<anygba::System> system;
    std::unique_ptr<anygba::save::SaveFile> save;
    try {
        anygba::input::Mapping mapping = load_input(*args);
        if (args->write_input) {
            std::ofstream out(*args->input_file, std::ios::binary | std::ios::trunc);
            out << anygba::input::format_mapping(mapping);
            out.close();
            if (!out) {
                throw std::runtime_error("cannot write " + args->input_file->string());
            }
            return 0;
        }

        std::vector<std::uint8_t> rom = read_rom(*args);
        if (anygba::sha256_hex(rom) != anygba_game_info.rom_sha256) {
            std::fprintf(stderr, "%s is not the ROM this program was generated from (expected SHA-256 %s)\n",
                         args->rom.empty() ? "the embedded ROM" : args->rom.c_str(), anygba_game_info.rom_sha256);
            return 1;
        }

        system = std::make_unique<anygba::System>(std::move(rom), &anygba_game_info, args->runtime);
        anygba::System& sys = *system;
        if (!args->no_save && sys.backup().type() != anygba::save::Type::None) {
            std::optional<fs::path> dir = args->save_dir;
            if (!dir) {
                dir = data_dir();
                if (dir) *dir /= "saves";
            }
            if (!dir) {
                throw std::runtime_error("no per-user data folder for the save file; pass --save-dir");
            }
            save = std::make_unique<anygba::save::SaveFile>(*dir / save_file_name(), sys.backup());
            save->load();
        }

        Frontend frontend(*args, sys, std::move(mapping), save.get());
        sys.on_vblank = [&frontend](anygba::System&) { frontend.on_vblank(); };
        bool idle = false;
        if (args->exit_on_idle) {
            sys.on_idle_loop = [&idle](anygba::System& s) {
                idle = true;
                s.stop();
            };
        }
        const std::uint64_t start = anyport::platform::now_ns();
        sys.run();
        const double elapsed = static_cast<double>(anyport::platform::now_ns() - start) / 1e9;
        int status = 0;
        if (save) {
            if (const auto error = save->flush()) {
                std::fprintf(stderr, "error: %s\n", error->c_str());
                status = 1;
            }
        }
        if (idle) {
            std::printf("idle loop at 0x%08x after %llu frames\n", sys.cpu().r[15],
                        static_cast<unsigned long long>(sys.frames()));
        } else {
            std::printf("stopped after %llu frames in %.2f s (%.2f frames/s)\n",
                        static_cast<unsigned long long>(sys.frames()), elapsed,
                        elapsed > 0 ? static_cast<double>(sys.frames()) / elapsed : 0.0);
        }
        print_registers(sys.cpu());
        if (args->frame_hash) {
            const auto bytes = std::as_bytes(std::span(sys.framebuffer()));
            std::printf("frame %llu hash %s\n", static_cast<unsigned long long>(sys.frames()),
                        anygba::sha256_hex({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}).c_str());
        }
        if (sys.fifo_underruns()) {
            std::printf("%llu DirectSound FIFO underruns\n", static_cast<unsigned long long>(sys.fifo_underruns()));
        }
        if (!sys.misses().empty()) {
            std::printf("%zu addresses ran in the interpreter (see %s)\n", sys.misses().size(),
                        args->runtime.miss_log.string().c_str());
        }
        if (save) {
            std::printf("save: %s (%s)\n", save->path().string().c_str(), anygba::save::name(sys.backup().type()));
        }
        return status;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        if (save) {
            if (const auto error = save->flush()) {
                std::fprintf(stderr, "error: %s\n", error->c_str());
            }
        }
        if (system) {
            print_registers(system->cpu());
        }
        return 1;
    }
}
