#include "anygba/sha256.h"
#include "anygba/system.hpp"

#include <anyport/platform/audio.h>
#include <anyport/platform/context.h>
#include <anyport/platform/input.h>
#include <anyport/platform/timing.h>
#include <anyport/platform/window.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
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

constexpr int kSampleRate = anygba::System::kAudioRate;

struct Args {
    std::string rom;
    bool headless = false;
    bool pace = true;  // headless runs unpaced unless --pace
    bool pace_given = false;
    int scale = 3;
    std::optional<std::uint64_t> frames;
    std::optional<double> seconds;
    bool exit_on_idle = false;
    anygba::Options runtime;
};

const char* const kUsage =
    "usage: game <rom.gba> [options]\n"
    "\n"
    "  --headless       no window and no audio (SDL dummy drivers); runs unpaced unless --pace\n"
    "  --pace           pace to real time (default with a window)\n"
    "  --fast           do not pace\n"
    "  --scale N        initial window size as a multiple of 240x160 (default 3)\n"
    "  --frames N       stop after N frames\n"
    "  --seconds S      stop after S seconds of wall-clock time\n"
    "  --exit-on-idle   stop at the first branch-to-self and print the registers\n"
    "  --miss-log FILE  where to append addresses the recompiled code missed (default anygba-miss.log)\n"
    "  --no-miss-log    do not write the miss log\n"
    "\n"
    "Keys: arrows, X = A, Z = B, Return = Start, Backspace = Select, Q/W = L/R; hold Tab to fast-forward.\n";

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
        } else if (arg == "--no-miss-log") {
            a.runtime.log_misses = false;
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
        } else if (!arg.starts_with("-") && a.rom.empty()) {
            a.rom = arg;
        } else {
            return std::nullopt;
        }
    }
    if (a.rom.empty()) {
        return std::nullopt;
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

// KEYINPUT bit order: A, B, Select, Start, Right, Left, Up, Down, R, L.
std::uint16_t gba_keys(const platform::PadState& pad) {
    using platform::Button;
    static constexpr Button kMap[10] = {Button::South,     Button::East,     Button::Back,      Button::Start,
                                        Button::DpadRight, Button::DpadLeft, Button::DpadUp,    Button::DpadDown,
                                        Button::RightShoulder, Button::LeftShoulder};
    std::uint16_t keys = 0;
    for (int i = 0; i < 10; ++i) {
        if (pad.held(kMap[i])) {
            keys = static_cast<std::uint16_t>(keys | (1u << i));
        }
    }
    return keys;
}

// Host side of a running game: window, audio, input, and pacing, all driven from VBlank.
class Frontend {
public:
    Frontend(const Args& args, anygba::System& sys) : args_(args), sys_(sys) {
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
        bool fast_forward = !args_.pace;
        if (input_) {
            if (platform::pump_events(*input_)) {
                sys_.stop();
            }
            sys_.set_keys(gba_keys(input_->state()));
            fast_forward = fast_forward || input_->key_held("Tab");
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
    std::unique_ptr<platform::Context> context_;
    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<platform::AudioStream> audio_;
    std::unique_ptr<platform::Input> input_;
    std::unique_ptr<platform::FramePacer> pacer_;
    std::vector<std::int16_t> sound_;
    std::uint64_t start_ns_ = 0;
};

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

    std::unique_ptr<anygba::System> system;
    try {
        system = std::make_unique<anygba::System>(std::move(rom), &anygba_game_info, args->runtime);
        anygba::System& sys = *system;
        Frontend frontend(*args, sys);
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
        if (idle) {
            std::printf("idle loop at 0x%08x after %llu frames\n", sys.cpu().r[15],
                        static_cast<unsigned long long>(sys.frames()));
        } else {
            std::printf("stopped after %llu frames in %.2f s (%.2f frames/s)\n",
                        static_cast<unsigned long long>(sys.frames()), elapsed,
                        elapsed > 0 ? static_cast<double>(sys.frames()) / elapsed : 0.0);
        }
        print_registers(sys.cpu());
        if (sys.fifo_underruns()) {
            std::printf("%llu DirectSound FIFO underruns\n", static_cast<unsigned long long>(sys.fifo_underruns()));
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
