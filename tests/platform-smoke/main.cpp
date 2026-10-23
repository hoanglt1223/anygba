// Shows a test pattern and plays a tone through the platform layer.
//   platform-smoke [--headless] [--frames N] [--scale N]
// Escape or closing the window quits. Held pad buttons light up the cells along the bottom.
// --headless runs on SDL's dummy drivers and verifies the scaled output, the audio stream
// drain, and keyboard routing, exiting non-zero on any mismatch.

#include <anyport/platform/audio.h>
#include <anyport/platform/context.h>
#include <anyport/platform/input.h>
#include <anyport/platform/log.h>
#include <anyport/platform/timing.h>
#include <anyport/platform/window.h>

#include <SDL3/SDL_events.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace pf = anyport::platform;

namespace {

constexpr int kWidth = 240;
constexpr int kHeight = 160;
constexpr int kSampleRate = 32768;
constexpr int kChannels = 2;
constexpr double kFrameRate = 60.0;
constexpr int kCellSize = 12;
constexpr int kBarAreaHeight = 112;

constexpr std::uint16_t rgb555(int r, int g, int b) {
    return static_cast<std::uint16_t>(r | (g << 5) | (b << 10));
}

constexpr std::uint16_t kBars[] = {
    rgb555(31, 31, 31), rgb555(31, 31, 0), rgb555(0, 31, 31), rgb555(0, 31, 0),
    rgb555(31, 0, 31),  rgb555(31, 0, 0),  rgb555(0, 0, 31),  rgb555(0, 0, 0),
};

struct Args {
    bool headless = false;
    int frames = -1;
    int scale = 3;
};

bool parse_int(std::string_view text, int& out) {
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && ptr == end;
}

bool parse_args(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--headless") {
            args.headless = true;
        } else if ((arg == "--frames" || arg == "--scale") && i + 1 < argc) {
            int value = 0;
            if (!parse_int(argv[++i], value) || value <= 0) {
                std::fprintf(stderr, "%s needs a positive integer\n", argv[i - 1]);
                return false;
            }
            (arg == "--frames" ? args.frames : args.scale) = value;
        } else {
            std::fprintf(stderr, "usage: platform-smoke [--headless] [--frames N] [--scale N]\n");
            return false;
        }
    }
    if (args.headless && args.frames < 0) {
        args.frames = 60;
    }
    return true;
}

// Colour bars, a 32-step grey ramp, one cell per pad button, and a square that moves each
// frame so a frozen display is obvious.
void draw_pattern(std::vector<std::uint16_t>& fb, int frame, const pf::PadState& pad) {
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            std::uint16_t c;
            if (y < kBarAreaHeight) {
                c = kBars[x * 8 / kWidth];
            } else if (y < kHeight - kCellSize - 4) {
                const int level = x * 32 / kWidth;
                c = rgb555(level, level, level);
            } else {
                c = 0;
            }
            fb[static_cast<std::size_t>(y * kWidth + x)] = c;
        }
    }
    for (int b = 0; b < static_cast<int>(pf::Button::Count); ++b) {
        const bool held = pad.held(static_cast<pf::Button>(b));
        const std::uint16_t c = held ? rgb555(31, 31, 0) : rgb555(6, 6, 6);
        const int x0 = 2 + b * (kCellSize + 2);
        for (int y = kHeight - kCellSize - 2; y < kHeight - 2; ++y) {
            for (int x = x0; x < x0 + kCellSize && x < kWidth; ++x) {
                fb[static_cast<std::size_t>(y * kWidth + x)] = c;
            }
        }
    }
    const int sx = (frame * 2) % (kWidth - 16);
    for (int y = 48; y < 64; ++y) {
        for (int x = sx; x < sx + 16; ++x) {
            fb[static_cast<std::size_t>(y * kWidth + x)] = rgb555(31, 15, 0);
        }
    }
}

class ToneGenerator {
public:
    // Keeps about three frames of audio queued.
    std::size_t fill(pf::AudioStream& stream) {
        const std::size_t target = static_cast<std::size_t>(kSampleRate / kFrameRate * 3);
        const std::size_t queued = stream.queued_frames();
        if (queued >= target) {
            return 0;
        }
        const std::size_t frames = target - queued;
        buffer_.resize(frames * kChannels);
        for (std::size_t i = 0; i < frames; ++i) {
            const double left = std::sin(phase_);
            const double right = std::sin(phase_ * 1.5);
            buffer_[i * 2] = static_cast<std::int16_t>(left * 6000.0);
            buffer_[i * 2 + 1] = static_cast<std::int16_t>(right * 6000.0);
            phase_ += 2.0 * std::numbers::pi * 440.0 / kSampleRate;
            if (phase_ > 4.0 * std::numbers::pi) {
                phase_ -= 4.0 * std::numbers::pi;
            }
        }
        stream.push(buffer_);
        return frames;
    }

private:
    double phase_ = 0.0;
    std::vector<std::int16_t> buffer_;
};

// Every output pixel must equal its framebuffer source pixel at an integer scale. SDL
// expands 5-bit channels to 8 bits slightly differently per renderer, so allow +/-8.
bool verify_scaled_output(const pf::Image& image, const std::vector<std::uint16_t>& fb,
                          int scale) {
    if (image.width != kWidth * scale || image.height != kHeight * scale) {
        std::fprintf(stderr, "output is %dx%d, expected %dx%d\n", image.width, image.height,
                     kWidth * scale, kHeight * scale);
        return false;
    }
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::uint16_t src = fb[static_cast<std::size_t>((y / scale) * kWidth + x / scale)];
            const int expected[3] = {(src & 31) * 255 / 31, ((src >> 5) & 31) * 255 / 31,
                                     ((src >> 10) & 31) * 255 / 31};
            const auto* px = &image.rgba[static_cast<std::size_t>((y * image.width + x) * 4)];
            for (int c = 0; c < 3; ++c) {
                if (std::abs(px[c] - expected[c]) > 8) {
                    std::fprintf(stderr, "pixel (%d,%d) channel %d is %d, expected %d\n", x, y, c,
                                 px[c], expected[c]);
                    return false;
                }
            }
        }
    }
    return true;
}

bool verify_keyboard_routing(pf::Input& input) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_X;
    event.key.down = true;
    if (!SDL_PushEvent(&event)) {
        std::fprintf(stderr, "SDL_PushEvent failed: %s\n", SDL_GetError());
        return false;
    }
    pf::pump_events(input);
    if (!input.state().held(pf::Button::South)) {
        std::fprintf(stderr, "key X did not reach Button::South\n");
        return false;
    }
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    SDL_PushEvent(&event);
    pf::pump_events(input);
    if (input.state().held(pf::Button::South)) {
        std::fprintf(stderr, "Button::South still held after key up\n");
        return false;
    }
    return true;
}

int run(const Args& args) {
    pf::Context context({.headless = args.headless});
    pf::Window window({.title = "platform-smoke",
                       .width = kWidth,
                       .height = kHeight,
                       .scale = args.scale,
                       .vsync = !args.headless});
    pf::AudioStream audio({.sample_rate = kSampleRate, .channels = kChannels});
    pf::Input input;
    pf::FramePacer pacer(kFrameRate);
    ToneGenerator tone;
    std::vector<std::uint16_t> fb(static_cast<std::size_t>(kWidth * kHeight));

    pf::log(pf::LogLevel::Info, "platform-smoke: %s, %d Hz stereo, %zu gamepad(s)",
            args.headless ? "headless" : "windowed", kSampleRate, input.gamepad_count());

    std::size_t pushed = 0;
    bool output_ok = true;
    const std::uint64_t start = pf::now_ns();
    int frame = 0;
    for (; args.frames < 0 || frame < args.frames; ++frame) {
        if (!pf::pump_events(input) || input.key_held("Escape")) {
            break;
        }
        draw_pattern(fb, frame, input.state());
        pushed += tone.fill(audio);
        if (args.headless && frame == 0) {
            pf::Image image;
            window.present(fb, &image);
            output_ok = verify_scaled_output(image, fb, args.scale);
        } else {
            window.present(fb);
        }
        pacer.wait();
    }
    const double seconds = static_cast<double>(pf::now_ns() - start) / 1e9;
    const std::size_t queued = audio.queued_frames();
    pf::log(pf::LogLevel::Info, "%d frames in %.2f s, %zu audio frames pushed, %zu still queued",
            frame, seconds, pushed, queued);

    if (!args.headless) {
        return 0;
    }
    bool ok = output_ok;
    if (pushed == 0 || queued >= pushed) {
        std::fprintf(stderr, "audio device consumed nothing (pushed %zu, queued %zu)\n", pushed,
                     queued);
        ok = false;
    }
    ok = verify_keyboard_routing(input) && ok;
    std::puts(ok ? "platform-smoke: PASS" : "platform-smoke: FAIL");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        return 2;
    }
    try {
        return run(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "platform-smoke: %s\n", e.what());
        return 1;
    }
}
