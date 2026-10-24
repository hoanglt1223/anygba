#include "anygba/crc32.h"
#include "anygba/embedded_rom.h"
#include "anygba/sha256.h"
#include "anygba/system.hpp"
#include "cheats/cheat_list.h"
#include "cheats/engine.h"
#include "input_map.h"
#include "player/menu.h"
#include "player/osd.h"
#include "player/script.h"
#include "player/speed.h"
#include "save/save.h"

#include <anyport/platform/audio.h>
#include <anyport/platform/context.h>
#include <anyport/platform/input.h>
#include <anyport/platform/timing.h>
#include <anyport/platform/window.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

extern "C" const anygba_game anygba_game_info;

namespace {

namespace platform = anyport::platform;
namespace fs = std::filesystem;
namespace player = anygba::player;
namespace cheats = anygba::cheats;
using anygba::input::Hotkey;

constexpr int kSampleRate = anygba::System::kAudioRate;

struct CheatArg {
    cheats::Format format = cheats::Format::Auto;
    std::vector<std::string> lines;
    std::string text;
};

struct Args {
    std::string rom;  // empty: the ROM embedded by `anygba build`
    bool headless = false;
    bool pace = true;  // headless runs unpaced unless --pace
    bool pace_given = false;
    int scale = 3;
    std::optional<int> speed;
    std::optional<std::uint64_t> frames;
    std::optional<double> seconds;
    bool exit_on_idle = false;
    bool frame_hash = false;
    std::optional<fs::path> save_dir;
    bool no_save = false;
    std::optional<fs::path> input_file;
    bool write_input = false;
    std::optional<fs::path> load_state;
    std::vector<fs::path> scripts;
    std::vector<std::string> keys;
    std::vector<CheatArg> cheats;
    std::vector<fs::path> cheat_files;
    std::vector<fs::path> cheat_dbs;
    std::vector<std::pair<std::string, fs::path>> dumps;
    anygba::Options runtime;
};

const char* const kUsage =
    "usage: game [rom.gba] [options]\n"
    "\n"
    "  rom.gba             the ROM this program was generated from; omit it when the ROM is embedded\n"
    "  --headless          no window and no audio (SDL dummy drivers); runs unpaced unless --pace\n"
    "  --pace              pace to real time (default with a window)\n"
    "  --fast              do not pace\n"
    "  --speed N           speed in percent: 100, 200, 300 or 400 (default: the input file's [player] Speed)\n"
    "  --scale N           initial window size as a multiple of 240x160 (default 3)\n"
    "  --frames N          stop after N frames\n"
    "  --seconds S         stop after S seconds of wall-clock time\n"
    "  --exit-on-idle      stop at the first branch-to-self and print the registers\n"
    "  --dump-frame-hash   print the SHA-256 of the last frame (RGB555, little-endian) on exit\n"
    "  --save-dir DIR      where the save file, save states and cheat file live (default: the per-user data folder)\n"
    "  --save-type T       none, sram, flash64, flash128, eeprom, eeprom512 or eeprom8k (default: config, else detect)\n"
    "  --no-save           do not read or write a save file\n"
    "  --load-state FILE   start from a save state\n"
    "  --cheat CODE        enable a cheat for this run: [FORMAT:]LINE;LINE... (FORMAT: auto, raw, gsv1, gsv1raw,\n"
    "                      arv3, arv3raw, cb); repeatable\n"
    "  --cheats FILE       enable the cheats of a cheat file for this run; repeatable\n"
    "  --cheat-db FILE     use a cheat database besides the ones found in cheats/ folders; repeatable\n"
    "  --dump REGION:FILE  write memory on exit; REGION is ewram, iwram, io, palette, vram, oam, or frame (the\n"
    "                      last picture, 240x160 RGB555 little-endian); repeatable\n"
    "  --input FILE        input mapping (default: anygba-input.ini next to the program, else in the data folder)\n"
    "  --write-input FILE  write the current input mapping to FILE and exit\n"
    "  --script FILE       scripted input (keys, gamepad, menu use) by host frame; repeatable\n"
    "  --keys SPEC         GBA buttons held over frame ranges, e.g. a:10-20,start:30-31\n"
    "  --miss-log FILE     where to append addresses the recompiled code missed (default anygba-miss.log)\n"
    "  --no-miss-log       do not write the miss log\n";

std::optional<CheatArg> parse_cheat_arg(std::string text) {
    CheatArg c;
    c.text = text;
    const auto colon = text.find(':');
    if (colon != std::string::npos) {
        if (const auto f = cheats::parse_format(text.substr(0, colon))) {
            c.format = *f;
            text = text.substr(colon + 1);
        }
    }
    c.lines = cheats::split_lines(text);
    if (c.lines.empty()) return std::nullopt;
    return c;
}

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
        } else if (arg == "--speed" && has_value) {
            const long n = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || !player::valid_speed(static_cast<int>(n))) return std::nullopt;
            a.speed = static_cast<int>(n);
        } else if (arg == "--miss-log" && has_value) {
            a.runtime.miss_log = argv[++i];
        } else if (arg == "--save-dir" && has_value) {
            a.save_dir = fs::path(argv[++i]);
        } else if (arg == "--save-type" && has_value) {
            a.runtime.save_type = argv[++i];
            if (!anygba::save::parse(a.runtime.save_type)) return std::nullopt;
        } else if (arg == "--load-state" && has_value) {
            a.load_state = fs::path(argv[++i]);
        } else if (arg == "--cheat" && has_value) {
            auto c = parse_cheat_arg(argv[++i]);
            if (!c) return std::nullopt;
            a.cheats.push_back(std::move(*c));
        } else if (arg == "--cheats" && has_value) {
            a.cheat_files.emplace_back(argv[++i]);
        } else if (arg == "--dump" && has_value) {
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            if (colon == std::string::npos) return std::nullopt;
            const std::string region = spec.substr(0, colon);
            if (region != "ewram" && region != "iwram" && region != "io" && region != "palette" && region != "vram" &&
                region != "oam" && region != "frame") {
                return std::nullopt;
            }
            a.dumps.emplace_back(region, fs::path(spec.substr(colon + 1)));
        } else if (arg == "--cheat-db" && has_value) {
            a.cheat_dbs.emplace_back(argv[++i]);
        } else if (arg == "--input" && has_value) {
            a.input_file = fs::path(argv[++i]);
        } else if (arg == "--write-input" && has_value) {
            a.input_file = fs::path(argv[++i]);
            a.write_input = true;
        } else if (arg == "--script" && has_value) {
            a.scripts.emplace_back(argv[++i]);
        } else if (arg == "--keys" && has_value) {
            a.keys.emplace_back(argv[++i]);
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

// "<title>-<hash8>": the stem of the save file, save states and cheat file.
std::string game_file_stem() {
    std::string name;
    for (const char* p = anygba_game_info.title; *p; ++p) {
        const char ch = *p;
        const bool safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                          ch == '_';
        name += safe ? ch : '_';
    }
    // The hash prefix keeps two builds that share a title (or have none) apart.
    return (name.empty() ? "game" : name) + "-" + std::string(anygba_game_info.rom_sha256).substr(0, 8);
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Writes through a temporary file so a crash never leaves half a file. Returns an error.
std::optional<std::string> write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) return "cannot write " + tmp.string();
    }
    fs::rename(tmp, path, ec);
    if (ec) return "cannot replace " + path.string() + ": " + ec.message();
    return std::nullopt;
}

struct LoadedMapping {
    anygba::input::Mapping mapping;
    std::optional<fs::path> path;  // where changes are saved
};

LoadedMapping load_input(const Args& args) {
    if (args.input_file && !args.write_input) {
        return {anygba::input::load_mapping(*args.input_file), args.input_file};
    }
    for (const auto& dir : {program_dir(), data_dir()}) {
        if (dir && fs::exists(*dir / "anygba-input.ini")) {
            return {anygba::input::load_mapping(*dir / "anygba-input.ini"), *dir / "anygba-input.ini"};
        }
    }
    const auto dir = data_dir();
    return {anygba::input::default_mapping(), dir ? std::optional<fs::path>(*dir / "anygba-input.ini") : std::nullopt};
}

class NoInput final : public anygba::input::Source {
public:
    bool key_held(std::string_view) const override { return false; }
    platform::PadState pad() const override { return {}; }
};

// Host side of a running game: window, audio, input, pacing, saves, save states, cheats and
// the menu, all driven from VBlank.
class Player {
public:
    Player(const Args& args, anygba::System& sys, LoadedMapping mapping, anygba::save::SaveFile* save,
           std::optional<fs::path> state_dir, cheats::CheatList& cheat_list, std::optional<fs::path> cheat_file,
           std::optional<player::Script> script)
        : args_(args),
          sys_(sys),
          mapping_(std::move(mapping.mapping)),
          mapping_path_(std::move(mapping.path)),
          save_(save),
          state_dir_(std::move(state_dir)),
          cheats_(cheat_list),
          cheat_file_(std::move(cheat_file)),
          script_(std::move(script)),
          osd_(args.headless),
          speed_(args.speed.value_or(mapping_.speed)),
          menu_(cheats_, mapping_, speed_, slot_) {
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
            device_ = std::make_unique<anygba::input::DeviceSource>(*input_);
            if (const auto dir = program_dir(); dir && fs::exists(*dir / "gamecontrollerdb.txt")) {
                try {
                    const int added = platform::add_gamepad_mappings((*dir / "gamecontrollerdb.txt").string());
                    std::printf("gamecontrollerdb.txt: %d gamepad mappings\n", added);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "warning: %s\n", e.what());
                }
            }
        }
        reset_pacer();
        start_ns_ = platform::now_ns();
        sync_rom_patches();
        const auto active = cheats_.active().size();
        if (active) {
            osd_.show(std::to_string(active) + (active == 1 ? " cheat on" : " cheats on"));
        }
        if (speed_ != 100) {
            osd_.show("Speed " + std::to_string(speed_) + "%");
        }
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
        if (!poll()) {
            sys_.stop();
            return;
        }
        const std::uint32_t held = anygba::input::hotkeys_held(mapping_, source());
        const std::uint32_t pressed = held & ~hotkeys_prev_;
        hotkeys_prev_ = held;
        handle_hotkeys(pressed);
        const std::uint16_t keys = gba_keys();
        sys_.set_keys(keys);

        if (cheats_.generation() != cheats_seen_) {
            sync_rom_patches();
        }
        const cheats::FrameKeys frame_keys{keys, anygba::input::has(held, Hotkey::CheatButton)};
        for (const cheats::Program* program : cheats_.active()) {
            cheats::run(*program, sys_, frame_keys);
        }

        const bool turbo = anygba::input::has(held, Hotkey::FastForward) || !args_.pace;
        present(turbo);
        sound_.clear();
        sys_.drain_audio(sound_);
        if (audio_) {
            feed_audio_and_wait(turbo);
        } else if (pacer_ && !turbo) {
            pacer_->wait();
        }
        osd_.tick();
        if (menu_.is_open()) {
            run_menu();
        }
    }

    // Starts from a state file before the first frame (--load-state).
    void load_state_file(const fs::path& path) {
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(read_bytes(path));
        sys_.request_safe_point([bytes, path](anygba::System& s) {
            try {
                s.load_state(*bytes);
            } catch (const std::exception& e) {
                throw std::runtime_error(path.string() + ": " + e.what());
            }
        });
    }

private:
    const anygba::input::Source& source() const {
        if (script_) return *script_;
        if (device_) return *device_;
        return no_input_;
    }

    std::uint16_t gba_keys() const {
        std::uint16_t keys = anygba::input::gba_keys(mapping_, source());
        if (script_) keys = static_cast<std::uint16_t>(keys | script_->gba_held());
        return keys;
    }

    // One host frame of input. Returns false when the window was closed.
    bool poll() {
        ++host_frame_;
        bool keep = true;
        std::vector<platform::GamepadEvent> events;
        if (input_) {
            keep = platform::pump_events(*input_);
            events = input_->take_gamepad_events();
            key_presses_ = input_->take_key_presses();
        }
        if (script_) {
            script_->advance(host_frame_);
            const auto scripted = script_->take_gamepad_events();
            events.insert(events.end(), scripted.begin(), scripted.end());
            key_presses_ = script_->take_key_presses();
        }
        for (const auto& e : events) {
            osd_.show(std::string(e.connected ? "Gamepad connected: " : "Gamepad removed: ") + e.name);
        }
        return keep;
    }

    void handle_hotkeys(std::uint32_t pressed) {
        for (int slot = 1; slot <= 9; ++slot) {
            if (anygba::input::has(pressed, anygba::input::save_slot_key(slot))) save_state(slot);
            if (anygba::input::has(pressed, anygba::input::load_slot_key(slot))) load_state(slot);
        }
        if (anygba::input::has(pressed, Hotkey::SpeedUp)) set_speed(player::next_speed(speed_, 1));
        if (anygba::input::has(pressed, Hotkey::SpeedDown)) set_speed(player::next_speed(speed_, -1));
        if (anygba::input::has(pressed, Hotkey::Menu)) {
            menu_.open();
            menu_trace_.clear();
        }
    }

    void set_speed(int speed) {
        speed_ = speed;
        reset_pacer();
        osd_.show("Speed " + std::to_string(speed_) + "%");
    }

    void reset_pacer() {
        pacer_.reset();
        if (args_.pace && !audio_) {
            pacer_ = std::make_unique<platform::FramePacer>(anygba::kFramesPerSecond * speed_ / 100.0);
        }
    }

    std::optional<fs::path> state_path(int slot) const {
        if (!state_dir_) return std::nullopt;
        return *state_dir_ / (game_file_stem() + ".ss" + std::to_string(slot));
    }

    void save_state(int slot) {
        const auto path = state_path(slot);
        if (!path) {
            osd_.show("No folder for save states; pass --save-dir");
            return;
        }
        sys_.request_safe_point([this, slot, path = *path](anygba::System& s) {
            try {
                if (const auto error = write_file(path, s.save_state())) {
                    osd_.show("Save state " + std::to_string(slot) + " failed: " + *error);
                } else {
                    osd_.show("Saved state " + std::to_string(slot));
                }
            } catch (const std::exception& e) {
                osd_.show("Save state " + std::to_string(slot) + " failed: " + e.what());
            }
        });
    }

    void load_state(int slot) {
        const auto path = state_path(slot);
        std::error_code ec;
        if (!path || !fs::exists(*path, ec)) {
            osd_.show("No save state in slot " + std::to_string(slot));
            return;
        }
        std::shared_ptr<std::vector<std::uint8_t>> bytes;
        try {
            bytes = std::make_shared<std::vector<std::uint8_t>>(read_bytes(*path));
        } catch (const std::exception& e) {
            osd_.show(e.what());
            return;
        }
        sys_.request_safe_point([this, slot, bytes](anygba::System& s) {
            try {
                s.load_state(*bytes);
                resampler_.reset();
                osd_.show("Loaded state " + std::to_string(slot));
            } catch (const std::exception& e) {
                osd_.show("Load state " + std::to_string(slot) + " failed: " + e.what());
            }
        });
    }

    void sync_rom_patches() {
        cheats_seen_ = cheats_.generation();
        const auto patches = cheats::rom_patches(cheats_.active());
        if (patches != sys_.rom_patches()) {
            sys_.set_rom_patches(patches);
        }
    }

    void save_cheats() {
        if (!cheat_file_) return;
        if (const auto error = cheats_.save_user(*cheat_file_, anygba_game_info.title)) {
            osd_.show(*error);
        }
    }

    void save_mapping() {
        if (!mapping_path_) {
            osd_.show("No folder for anygba-input.ini");
            return;
        }
        if (const auto error = anygba::input::save_mapping(mapping_, *mapping_path_)) {
            osd_.show(*error);
        }
    }

    // The game is paused: the menu runs one host frame at a time until it closes.
    void run_menu() {
        std::uint32_t pad_prev = source().pad().buttons;
        const std::uint64_t script_end = script_ ? script_->last_frame() : 0;
        while (menu_.is_open()) {
            if (!poll()) {
                sys_.stop();
                return;
            }
            const std::uint32_t held = anygba::input::hotkeys_held(mapping_, source());
            const std::uint32_t pressed_hotkeys = held & ~hotkeys_prev_;
            hotkeys_prev_ = held;

            player::MenuInput in;
            in.gba_held = gba_keys();
            in.keys = std::move(key_presses_);
            const platform::PadState pad = source().pad();
            in.pad_held = pad.buttons;
            for (int b = 0; b < static_cast<int>(platform::Button::Count); ++b) {
                if (((pad.buttons & ~pad_prev) >> b) & 1u) in.pads.push_back(static_cast<platform::Button>(b));
            }
            pad_prev = pad.buttons;
            const anygba::input::Source& src = source();
            in.ctrl = src.key_held("Left Ctrl") || src.key_held("Right Ctrl");
            in.alt = src.key_held("Left Alt") || src.key_held("Right Alt");
            in.shift = src.key_held("Left Shift") || src.key_held("Right Shift");
            // Only a gamepad chord closes the menu through the hotkey; Escape arrives as a key.
            in.menu_hotkey = anygba::input::has(pressed_hotkeys, Hotkey::Menu) &&
                             std::find(in.keys.begin(), in.keys.end(), "Escape") == in.keys.end();

            for (const player::MenuAction action : menu_.update(in)) {
                switch (action) {
                case player::MenuAction::SaveState: save_state(slot_); break;
                case player::MenuAction::LoadState: load_state(slot_); break;
                case player::MenuAction::SpeedChanged: set_speed(speed_); break;
                case player::MenuAction::CheatsChanged:
                    sync_rom_patches();
                    save_cheats();
                    break;
                case player::MenuAction::MappingChanged: save_mapping(); break;
                case player::MenuAction::Close: break;
                }
            }
            trace_menu();
            osd_.tick();
            if (window_) {
                frame_copy_ = sys_.framebuffer();
                menu_.draw(frame_copy_);
                osd_.draw(frame_copy_);
                window_->present(frame_copy_);
                platform::sleep_ns(16'000'000);
            }
            if (args_.headless && script_ && host_frame_ > script_end + 600) {
                std::fprintf(stderr, "warning: the script ended with the menu open; closing it\n");
                menu_.close();
            }
            if (args_.headless && !script_) {
                menu_.close();  // nothing could ever close it
            }
        }
        trace_menu();
        hotkeys_prev_ = anygba::input::hotkeys_held(mapping_, source());
        resampler_.reset();
    }

    // Headless runs print the menu whenever it changes, for scripted tests and debugging.
    void trace_menu() {
        if (!args_.headless) return;
        std::vector<std::string> lines = menu_.is_open() ? menu_.lines() : std::vector<std::string>{"(closed)"};
        if (lines == menu_trace_) return;
        menu_trace_ = lines;
        for (const auto& l : lines) std::printf("menu| %s\n", l.c_str());
        std::fflush(stdout);
    }

    void present(bool turbo) {
        if (!window_) return;
        const std::uint64_t now = platform::now_ns();
        if ((turbo || speed_ > 100) && now - last_present_ns_ < 15'000'000) {
            return;  // fast play: show at most about 60 pictures a second
        }
        last_present_ns_ = now;
        if (osd_.empty()) {
            window_->present(sys_.framebuffer());
            return;
        }
        frame_copy_ = sys_.framebuffer();
        osd_.draw(frame_copy_);
        window_->present(frame_copy_);
    }

    // Audio-led pacing: the device consumes 32768 frames per second of sound (SDL resamples
    // to the device rate), so keeping the queue near two video frames ties emulation speed to
    // the sound card's clock. Faster speeds squeeze each frame's sound by the speed factor, so
    // the queue still drains at the same rate and the game runs that much faster.
    void feed_audio_and_wait(bool turbo) {
        const std::size_t target = 2 * kSampleRate / 60;
        squeezed_.clear();
        resampler_.process(sound_, turbo ? 4.0 : speed_ / 100.0, squeezed_);
        if (turbo && audio_->queued_frames() > target) {
            return;  // unthrottled: drop what the device cannot play
        }
        audio_->push(squeezed_);
        while (!turbo && audio_->queued_frames() > target) {
            platform::sleep_ns(1'000'000);
        }
    }

    const Args& args_;
    anygba::System& sys_;
    anygba::input::Mapping mapping_;
    std::optional<fs::path> mapping_path_;
    anygba::save::SaveFile* save_;
    std::optional<fs::path> state_dir_;
    cheats::CheatList& cheats_;
    std::optional<fs::path> cheat_file_;
    std::optional<player::Script> script_;
    player::Osd osd_;
    int speed_ = 100;
    int slot_ = 1;
    player::Menu menu_;
    std::unique_ptr<platform::Context> context_;
    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<platform::AudioStream> audio_;
    std::unique_ptr<platform::Input> input_;
    std::unique_ptr<anygba::input::DeviceSource> device_;
    NoInput no_input_;
    std::unique_ptr<platform::FramePacer> pacer_;
    player::Resampler resampler_;
    std::vector<std::int16_t> sound_;
    std::vector<std::int16_t> squeezed_;
    std::array<std::uint16_t, anygba::kScreenWidth * anygba::kScreenHeight> frame_copy_{};
    std::vector<std::string> key_presses_;
    std::vector<std::string> menu_trace_;
    std::uint32_t hotkeys_prev_ = 0;
    std::uint64_t host_frame_ = 0;
    std::uint64_t cheats_seen_ = ~std::uint64_t{0};
    std::uint64_t start_ns_ = 0;
    std::uint64_t last_present_ns_ = 0;
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

// Cheat databases for this ROM: <code>.toml and <crc32>.toml in cheats/ next to the program
// and in the data folder, then --cheat-db files.
void load_cheat_databases(const Args& args, const std::vector<std::uint8_t>& rom, cheats::CheatList& list) {
    const std::uint32_t crc = anygba::crc32(rom);
    std::string code(rom.begin() + 0xAC, rom.begin() + 0xB0);
    for (char& ch : code) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
    }
    char crc_name[16];
    std::snprintf(crc_name, sizeof crc_name, "%08x", crc);
    // <code>.toml, <crc32>.toml, and numbered variants such as <crc32>-2.toml.
    const auto ours = [&](const fs::path& p) {
        if (p.extension() != ".toml") return false;
        const std::string stem = p.stem().string();
        for (const std::string& key : {code, std::string(crc_name)}) {
            if (stem == key || (stem.size() > key.size() + 1 && stem.compare(0, key.size() + 1, key + "-") == 0)) {
                return true;
            }
        }
        return false;
    };
    std::vector<fs::path> files;
    for (const auto& dir : {program_dir(), data_dir()}) {
        std::error_code ec;
        if (!dir || !fs::is_directory(*dir / "cheats", ec)) continue;
        std::vector<fs::path> found;
        for (const auto& entry : fs::directory_iterator(*dir / "cheats", ec)) {
            if (entry.is_regular_file(ec) && ours(entry.path())) found.push_back(entry.path());
        }
        std::sort(found.begin(), found.end());
        files.insert(files.end(), found.begin(), found.end());
    }
    files.insert(files.end(), args.cheat_dbs.begin(), args.cheat_dbs.end());
    for (const fs::path& f : files) {
        if (!list.load_database(f, crc)) {
            std::fprintf(stderr, "warning: %s is for another version of this game (CRC-32 %08x); skipped\n",
                         f.string().c_str(), crc);
        }
    }
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
        LoadedMapping mapping = load_input(*args);
        if (args->write_input) {
            std::ofstream out(*args->input_file, std::ios::binary | std::ios::trunc);
            out << anygba::input::format_mapping(mapping.mapping);
            out.close();
            if (!out) {
                throw std::runtime_error("cannot write " + args->input_file->string());
            }
            return 0;
        }

        std::optional<player::Script> script;
        for (const auto& path : args->scripts) {
            auto s = player::Script::parse(read_text(path), path.string());
            if (!script) script.emplace();
            script->append(s);
        }
        for (const auto& spec : args->keys) {
            if (!script) script.emplace();
            script->append(player::Script::from_keys(spec));
        }

        std::vector<std::uint8_t> rom = read_rom(*args);
        if (anygba::sha256_hex(rom) != anygba_game_info.rom_sha256) {
            std::fprintf(stderr, "%s is not the ROM this program was generated from (expected SHA-256 %s)\n",
                         args->rom.empty() ? "the embedded ROM" : args->rom.c_str(), anygba_game_info.rom_sha256);
            return 1;
        }

        cheats::CheatList cheat_list;
        load_cheat_databases(*args, rom, cheat_list);

        system = std::make_unique<anygba::System>(std::move(rom), &anygba_game_info, args->runtime);
        anygba::System& sys = *system;
        std::optional<fs::path> dir = args->save_dir;
        if (!dir) {
            dir = data_dir();
            if (dir) *dir /= "saves";
        }
        if (!args->no_save && sys.backup().type() != anygba::save::Type::None) {
            if (!dir) {
                throw std::runtime_error("no per-user data folder for the save file; pass --save-dir");
            }
            save = std::make_unique<anygba::save::SaveFile>(*dir / (game_file_stem() + ".sav"), sys.backup());
            save->load();
        }

        std::optional<fs::path> cheat_file;
        if (dir) {
            cheat_file = *dir / (game_file_stem() + ".cheats.toml");
            cheat_list.load_user(*cheat_file);
        }
        for (const auto& f : args->cheat_files) {
            if (!fs::exists(f)) throw std::runtime_error("cannot open cheat file " + f.string());
            cheat_list.load_user(f, false);
        }
        for (const auto& c : args->cheats) {
            try {
                cheat_list.add_custom("--cheat " + c.text, c.format, c.lines, false);
            } catch (const cheats::CodeError& e) {
                throw std::runtime_error("--cheat " + c.text + ": " + e.what());
            }
        }
        for (const auto& w : cheat_list.warnings()) {
            std::fprintf(stderr, "warning: %s\n", w.c_str());
        }

        Player player(*args, sys, std::move(mapping), save.get(), dir, cheat_list, cheat_file, std::move(script));
        if (args->load_state) {
            player.load_state_file(*args->load_state);
        }
        sys.on_vblank = [&player](anygba::System&) { player.on_vblank(); };
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
        for (const auto& [region, path] : args->dumps) {
            if (region == "frame") {
                const auto bytes = std::as_bytes(std::span(sys.framebuffer()));
                const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
                if (const auto error = write_file(path, std::vector<std::uint8_t>(p, p + bytes.size()))) {
                    std::fprintf(stderr, "error: %s\n", error->c_str());
                    status = 1;
                }
                continue;
            }
            const anygba::Memory& mem = sys.memory();
            const std::vector<std::uint8_t>& data = region == "ewram"     ? mem.ewram
                                                    : region == "iwram"   ? mem.iwram
                                                    : region == "io"      ? mem.io
                                                    : region == "palette" ? mem.palette
                                                    : region == "vram"    ? mem.vram
                                                                          : mem.oam;
            if (const auto error = write_file(path, data)) {
                std::fprintf(stderr, "error: %s\n", error->c_str());
                status = 1;
            }
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
