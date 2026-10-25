#include "anygba/crc32.h"
#include "anygba/embedded_rom.h"
#include "anygba/sha256.h"
#include "anygba/system.hpp"
#include "cheats/cheat_list.h"
#include "cheats/engine.h"
#include "control/controller.h"
#include "control/net.h"
#include "input_map.h"
#include "overrides/difftest.h"
#include "overrides/overrides.h"
#include "player/lifecycle.h"
#include "journal/journal.h"
#include "player/dialog.h"
#include "player/menu.h"
#include "player/mp2k.h"
#include "player/osd.h"
#include "player/script.h"
#include "player/speed.h"
#include "player/stats.h"
#include "player/display_timing.h"
#include "video/motion.h"
#include "player/touch_overlay.h"
#include "save/save.h"
#include "ui/menu_assets.h"
#include "video/hd/png_io.h"
#include "video/hd/session.h"
#include "video/presenter.h"
#include "video/settings.h"

#include <anyport/platform/audio.h>
#include <anyport/platform/context.h>
#include <anyport/platform/input.h>
#include <anyport/platform/timing.h>
#include <anyport/platform/window.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_system.h>
#if defined(__ANDROID__)
#include <SDL3/SDL_main.h>  // main() becomes SDL_main(), which SDLActivity calls
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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

fs::path utf8_arg(const char* s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s))); }

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
    std::optional<int> fast_forward;
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
    std::optional<fs::path> wav;
    std::optional<fs::path> import_save;
    std::optional<fs::path> export_save;
    std::string video;  // --filter and --video, serialized
    std::optional<fs::path> hd_pack;
    std::optional<fs::path> hd_dump;
    anygba::video::hd::DumpFilter dump_filter;
    anygba::video::hd::ai::Device ai_device = anygba::video::hd::ai::Device::Auto;
    std::vector<std::pair<std::uint64_t, fs::path>> screenshots;
    std::pair<int, int> screenshot_size{0, 0};
    anygba::Options runtime;
    // Agent control and the play journal (docs/user/AI-CONTROL.md).
    bool control = false;
    bool control_readonly = false;
    std::string control_host = "127.0.0.1";
    std::uint16_t control_port = 0;
    std::optional<fs::path> control_dir;
    int takeover_frames = 180;
    bool journal = false;
    std::optional<fs::path> journal_dir;
    anygba::journal::Settings journal_settings;
    std::vector<fs::path> symbol_files;
    anygba::overrides::Mode overrides = anygba::overrides::Mode::On;
    // Differential tests of overrides (docs/dev/OVERRIDES.md).
    bool difftest = false;
    bool difftest_option = false;  // a difftest-only option was given
    anygba::difftest::Options difftest_options;
    std::optional<std::string> record_entry;
    std::uint64_t record_count = 100;
    std::optional<fs::path> record_out;
};

const char* const kUsage =
    "usage: game [rom.gba] [options]\n"
    "\n"
    "  rom.gba             the ROM this program was generated from; omit it when the ROM is embedded\n"
    "  --headless          no window and no audio (SDL dummy drivers); runs unpaced unless --pace\n"
    "  --pace              pace to real time (default with a window)\n"
    "  --fast              do not pace\n"
    "  --speed N           speed in percent: 100, 200, 300 or 400 (default: the input file's [player] Speed)\n"
    "  --ff-speed N        fast-forward speed while the hotkey is held: 150, 200, 300, 400, 500, 800 or\n"
    "                      unlimited (default: the input file's [player] FastForwardSpeed, else 300)\n"
    "  --scale N           initial window size as a multiple of 240x160 (default 3)\n"
    "  --filter SPEC       video filter: a scaler, a screen effect and/or ghosting, e.g. xbrz4x+lcd-grid\n"
    "                      (docs/user/VIDEO.md)\n"
    "  --video SETTINGS    video settings key=value;...: filter, color, gamma, deband, output, fullscreen,\n"
    "                      integer-scale, internal-scale, layer-filter, smooth-affine, hd-pack, hd-dump, ai,\n"
    "                      ai-model, widescreen (docs/user/VIDEO.md, HD-PACKS.md, WIDESCREEN.md)\n"
    "  --hd-pack DIR       use the HD pack in DIR (default: hdpacks/<game code>/ next to the program or in the\n"
    "                      data folder)\n"
    "  --hd-dump DIR       dump every tile, sprite and bitmap the game shows to DIR\n"
    "  --hd-dump-filter F  what to dump: all, tiles, objs, bitmaps, bg0-bg3, blank (comma-separated)\n"
    "  --ai-device D       AI upscaling on auto, dml, cuda or cpu (default auto)\n"
    "  --screenshot N:FILE write the picture of frame N as a PNG; repeatable\n"
    "  --screenshot-size WxH  size of --screenshot pictures (default: the picture's own size)\n"
    "  --frames N          stop after N frames\n"
    "  --seconds S         stop after S seconds of wall-clock time\n"
    "  --exit-on-idle      stop at the first branch-to-self and print the registers\n"
    "  --dump-frame-hash   print the SHA-256 of the last frame (RGB555, little-endian) on exit\n"
    "  --save-dir DIR      where the save file, save states and cheat file live (default: [player] SaveFolder, else the data folder)\n"
    "  --save-type T       none, sram, flash64, flash128, eeprom, eeprom512 or eeprom8k (default: config, else detect)\n"
    "  --no-save           do not read or write a save file\n"
    "  --rtc on|off        cartridge real-time clock (default: config, else on for games that had one)\n"
    "  --rtc-start T       start the clock at Unix time T, advancing with emulated time (default: host clock)\n"
    "  --load-state FILE   start from a save state\n"
    "  --cheat CODE        enable a cheat for this run: [FORMAT:]LINE;LINE... (FORMAT: auto, raw, gsv1, gsv1raw,\n"
    "                      arv3, arv3raw, cb); repeatable\n"
    "  --cheats FILE       enable the cheats of a cheat file for this run; repeatable\n"
    "  --cheat-db FILE     use a cheat database besides the ones found in cheats/ folders; repeatable\n"
    "  --dump REGION:FILE  write memory on exit; REGION is ewram, iwram, io, palette, vram, oam, or frame (the\n"
    "                      last picture, 240x160 RGB555 little-endian); repeatable\n"
    "  --wav FILE          record the sound as played (after speed changes) to a WAV file, also headless\n"
    "  --import-save FILE  load a .sav from another emulator (VisualBoyAdvance, mGBA) before starting; the current\n"
    "                      save is kept as <save>.before-import-<time>\n"
    "  --export-save FILE  write the save chip contents as a .sav for other emulators on exit\n"
    "  --input FILE        input mapping (default: anygba-input.ini next to the program, else in the data folder)\n"
    "  --write-input FILE  write the current input mapping to FILE and exit\n"
    "  --script FILE       scripted input (keys, gamepad, menu use) by host frame; repeatable\n"
    "  --keys SPEC         GBA buttons held over frame ranges, e.g. a:10-20,start:30-31\n"
    "  --miss-log FILE     where to append addresses the recompiled code missed (default anygba-miss.log)\n"
    "  --no-miss-log       do not write the miss log\n"
    "  --overrides MODE    function overrides compiled into the game: on (the enabled ones, default), off\n"
    "                      (the original code everywhere, for comparison) or all (also the disabled ones)\n"
    "\n"
    "Differential tests of overrides (docs/dev/OVERRIDES.md; `anygba difftest` wraps these):\n"
    "  --difftest FUNC     compare override FUNC (symbol or address) with its translation, print the report and\n"
    "                      exit: 0 pass, 1 fail; random states start from --load-state FILE (default: reset)\n"
    "  --spec FILE         argument spec for random states (the module's difftest.toml)\n"
    "  --states N          random states (default 10000)      --seed N   random seed (default 1)\n"
    "  --recorded FILE     also test states written by --record-entry; repeatable\n"
    "  --impl NAME         test another implementation compiled into the program\n"
    "  --tick-limit N      a run longer than N scanlines of cycles is stuck (default: the spec's, else 10000)\n"
    "  --json FILE         write the report as JSON (- for stdout)\n"
    "  --record-entry FUNC record the state at each entry of FUNC (override name or address) during the run\n"
    "  --count N           stop after N recorded states (default 100)\n"
    "  --record-out FILE   where --record-entry writes the states\n"
    "\n"
    "AI agent control and the play journal (both can also be switched in the menu: Settings):\n"
    "  --control           start the control server (127.0.0.1 only, token in the per-user control folder)\n"
    "  --control-port N    port for the control server (default: any free port)\n"
    "  --control-host H    127.0.0.1 or localhost; any other address is refused\n"
    "  --control-dir DIR   where the token file is written (default: <data folder>/control)\n"
    "  --control-readonly  agents may only observe and debug\n"
    "  --takeover-frames N a user keypress pauses agent input for N frames (default 180)\n"
    "  --journal           record the play journal from the start\n"
    "  --journal-dir DIR   journal folder (default: <data folder>/journal)\n"
    "  --journal-snapshot S  named-symbol snapshots every S emulated seconds (default 10, 0 = off)\n"
    "  --journal-thumbnail S pictures every S emulated seconds (default 30, 0 = only on events)\n"
    "  --journal-max-mb N  keep at most N MB of journals (default 512)\n"
    "  --journal-days N    delete journals older than N days (default 30, 0 = never)\n"
    "  --symbols FILE      named memory for agents ([symbols] table); repeatable\n";

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
        } else if (arg == "--ff-speed" && has_value) {
            a.fast_forward = player::parse_fast_forward(argv[++i]);
            if (!a.fast_forward) return std::nullopt;
        } else if (arg == "--miss-log" && has_value) {
            a.runtime.miss_log = argv[++i];
        } else if (arg == "--save-dir" && has_value) {
            a.save_dir = fs::path(argv[++i]);
        } else if (arg == "--save-type" && has_value) {
            a.runtime.save_type = argv[++i];
            if (!anygba::save::parse(a.runtime.save_type)) return std::nullopt;
        } else if (arg == "--rtc" && has_value) {
            a.runtime.rtc = argv[++i];
            if (a.runtime.rtc != "on" && a.runtime.rtc != "off") return std::nullopt;
        } else if (arg == "--rtc-start" && has_value) {
            const long long t = std::strtoll(argv[++i], &end, 10);
            if (*end != '\0' || t < 946684800 || t > 4102444799) return std::nullopt;  // 2000-2099
            a.runtime.rtc_start = t;
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
        } else if (arg == "--export-save" && has_value) {
            a.export_save = fs::path(argv[++i]);
        } else if (arg == "--import-save" && has_value) {
            a.import_save = fs::path(argv[++i]);
        } else if (arg == "--wav" && has_value) {
            a.wav = fs::path(argv[++i]);
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
        } else if ((arg == "--filter" || arg == "--video") && has_value) {
            a.video += (a.video.empty() ? "" : ";") + std::string(arg == "--filter" ? "filter=" : "") + argv[++i];
        } else if (arg == "--control") {
            a.control = true;
        } else if (arg == "--control-readonly") {
            a.control_readonly = true;
        } else if (arg == "--control-host" && has_value) {
            a.control_host = argv[++i];
        } else if (arg == "--control-port" && has_value) {
            const long n = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || n < 1 || n > 65535) return std::nullopt;
            a.control_port = static_cast<std::uint16_t>(n);
        } else if (arg == "--control-dir" && has_value) {
            a.control_dir = fs::path(argv[++i]);
        } else if (arg == "--takeover-frames" && has_value) {
            const long n = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || n < 0 || n > 36000) return std::nullopt;
            a.takeover_frames = static_cast<int>(n);
        } else if (arg == "--journal") {
            a.journal = true;
        } else if (arg == "--journal-dir" && has_value) {
            a.journal_dir = fs::path(argv[++i]);
        } else if ((arg == "--journal-snapshot" || arg == "--journal-thumbnail" || arg == "--journal-days" ||
                    arg == "--journal-max-mb") && has_value) {
            const long n = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || n < 0 || n > 1000000) return std::nullopt;
            if (arg == "--journal-snapshot") a.journal_settings.snapshot_seconds = static_cast<int>(n);
            if (arg == "--journal-thumbnail") a.journal_settings.thumbnail_seconds = static_cast<int>(n);
            if (arg == "--journal-days") a.journal_settings.max_days = static_cast<int>(n);
            if (arg == "--journal-max-mb") {
                if (n == 0) return std::nullopt;
                a.journal_settings.max_mb = static_cast<std::uint64_t>(n);
            }
        } else if (arg == "--overrides" && has_value) {
            const auto mode = anygba::overrides::parse_mode(argv[++i]);
            if (!mode) return std::nullopt;
            a.overrides = *mode;
        } else if (arg == "--symbols" && has_value) {
            a.symbol_files.emplace_back(argv[++i]);
        } else if (arg == "--difftest" && has_value) {
            a.difftest = true;
            a.difftest_options.function = argv[++i];
        } else if (arg == "--spec" && has_value) {
            a.difftest_option = true;
            a.difftest_options.spec = utf8_arg(argv[++i]);
        } else if (arg == "--recorded" && has_value) {
            a.difftest_option = true;
            a.difftest_options.recorded.push_back(utf8_arg(argv[++i]));
        } else if (arg == "--impl" && has_value) {
            a.difftest_option = true;
            a.difftest_options.impl = argv[++i];
        } else if (arg == "--json" && has_value) {
            a.difftest_option = true;
            a.difftest_options.json = utf8_arg(argv[++i]);
        } else if ((arg == "--states" || arg == "--seed" || arg == "--tick-limit" || arg == "--count") && has_value) {
            const unsigned long long n = std::strtoull(argv[++i], &end, 10);
            if (*end != '\0' || argv[i][0] == '-') return std::nullopt;
            if (arg == "--states") a.difftest_options.states = n;
            if (arg == "--seed") a.difftest_options.seed = n;
            if (arg == "--tick-limit") a.difftest_options.tick_limit = n;
            if (arg == "--count") a.record_count = n;
            if (arg != "--count") a.difftest_option = true;
        } else if (arg == "--record-entry" && has_value) {
            a.record_entry = argv[++i];
        } else if (arg == "--record-out" && has_value) {
            a.record_out = utf8_arg(argv[++i]);
        } else if ((arg == "--hd-pack" || arg == "--hd-dump") && has_value) {
            (arg == "--hd-pack" ? a.hd_pack : a.hd_dump) = utf8_arg(argv[++i]);
            a.video += std::string(a.video.empty() ? "" : ";") + (arg == "--hd-pack" ? "hd-pack=true" : "hd-dump=true");
        } else if (arg == "--hd-dump-filter" && has_value) {
            if (const auto error = anygba::video::hd::parse_dump_filter(argv[++i], a.dump_filter)) {
                std::fprintf(stderr, "--hd-dump-filter: %s\n", error->c_str());
                return std::nullopt;
            }
        } else if (arg == "--ai-device" && has_value) {
            const auto d = anygba::video::hd::ai::parse_device(argv[++i]);
            if (!d) return std::nullopt;
            a.ai_device = *d;
        } else if (arg == "--screenshot" && has_value) {
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            const unsigned long long n = std::strtoull(spec.substr(0, colon).c_str(), &end, 10);
            if (colon == std::string::npos || colon == 0 || *end != '\0' || n == 0) return std::nullopt;
            a.screenshots.emplace_back(n, utf8_arg(spec.substr(colon + 1).c_str()));
        } else if (arg == "--screenshot-size" && has_value) {
            const std::string spec = argv[++i];
            const auto x = spec.find('x');
            const long w = std::strtol(spec.substr(0, x).c_str(), &end, 10);
            const bool w_ok = *end == '\0';
            const long h = x == std::string::npos ? 0 : std::strtol(spec.substr(x + 1).c_str(), &end, 10);
            if (!w_ok || *end != '\0' || w < 1 || h < 1 || w > 16384 || h > 16384) return std::nullopt;
            a.screenshot_size = {static_cast<int>(w), static_cast<int>(h)};
        } else if (!arg.starts_with("-") && a.rom.empty()) {
            a.rom = arg;
        } else {
            return std::nullopt;
        }
    }
    if (a.headless && !a.pace_given) {
        a.pace = false;
    }
    if ((a.difftest_option && !a.difftest) || a.record_entry.has_value() != a.record_out.has_value() ||
        (a.difftest && a.record_entry) || a.record_count == 0) {
        return std::nullopt;  // difftest options need --difftest; --record-entry and --record-out go together
    }
    if (a.difftest) {
        a.difftest_options.base_state = a.load_state;
        a.runtime.log_misses = false;
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

std::string path_utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

// Per-user data folder (created by SDL), e.g. %APPDATA%/anyport/anygba/ on Windows.
// ANYGBA_DATA_DIR replaces it (tests use it to keep the user's own files out).
std::optional<fs::path> data_dir() {
    if (const char* forced = SDL_getenv("ANYGBA_DATA_DIR"); forced && *forced) {
        fs::path dir = utf8_path(forced);
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }
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
    return anygba::save::replace_file(path, bytes, false);
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

// 16-bit stereo PCM WAV at the audio rate, sizes patched in by finish().
class WavWriter {
public:
    explicit WavWriter(const fs::path& path) : path_(path), out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) throw std::runtime_error("cannot write " + path.string());
        header(0);
    }
    void write(std::span<const std::int16_t> samples) {
        for (const std::int16_t s : samples) {
            const auto u = static_cast<std::uint16_t>(s);
            const char bytes[2] = {static_cast<char>(u & 0xFF), static_cast<char>(u >> 8)};
            out_.write(bytes, 2);
        }
        bytes_ += static_cast<std::uint32_t>(samples.size() * 2);
    }
    // Returns an error message.
    std::optional<std::string> finish() {
        out_.seekp(0);
        header(bytes_);
        out_.close();
        if (!out_) return "cannot write " + path_.string();
        return std::nullopt;
    }

private:
    void header(std::uint32_t data_bytes) {
        const auto u32 = [this](std::uint32_t v) {
            const char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16),
                               static_cast<char>(v >> 24)};
            out_.write(b, 4);
        };
        const auto u16 = [this](std::uint16_t v) {
            const char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
            out_.write(b, 2);
        };
        out_.write("RIFF", 4);
        u32(36 + data_bytes);
        out_.write("WAVEfmt ", 8);
        u32(16);
        u16(1);  // PCM
        u16(2);
        u32(kSampleRate);
        u32(kSampleRate * 4);
        u16(4);
        u16(16);
        out_.write("data", 4);
        u32(data_bytes);
    }

    fs::path path_;
    std::ofstream out_;
    std::uint32_t bytes_ = 0;
};

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
        file_fast_forward_ = mapping_.fast_forward;
        if (args.fast_forward) mapping_.fast_forward = *args.fast_forward;
        std::string video_warning;
        video_options_ = anygba::video::resolve_settings(anygba_game_info.video, video_file(), args.video, &video_warning);
        if (!video_warning.empty()) std::fprintf(stderr, "warning: %s\n", video_warning.c_str());
        menu_.set_video(&video_options_);
        menu_.set_game_title(anygba_game_info.title);
        anygba::overrides::set_osd_sink([this](const std::string& text) { osd_.show(text); });
        if (!args.headless) {
            context_ = std::make_unique<platform::Context>(platform::ContextOptions{false});
            platform::WindowConfig window;
            window.title = anygba_game_info.title;
            window.width = anygba::kScreenWidth;
            window.height = anygba::kScreenHeight;
            window.scale = args.scale;
            window.vsync = false;  // pacing follows the audio clock, not the display
            window_ = std::make_unique<platform::Window>(window);
            video_ = std::make_unique<anygba::video::Presenter>(*window_, video_options_);
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
#if defined(__ANDROID__)
            const auto data = data_dir();
            touch_ = std::make_unique<player::TouchOverlay>(data ? std::optional<fs::path>(*data / "anygba-touch.ini")
                                                                 : std::nullopt);
            window_->set_overlay(touch_.get());
#endif
            background_ = std::make_unique<player::BackgroundWatch>([this] { write_save(); });
        }
        if (args.wav) wav_ = std::make_unique<WavWriter>(*args.wav);
        hd_ = std::make_unique<anygba::video::hd::Session>(hd_setup());
        hd_->set_options(video_options_);
        sys_.on_line_rendered = [this](anygba::System& s, unsigned line) {
            hd_->on_line(s, line);
            if (smoothing_wanted()) motion_->on_line(s, line);
        };
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

    ~Player() {
        sys_.on_line_rendered = nullptr;
        anygba::overrides::set_osd_sink(nullptr);
    }

    // One line of HD timing on exit, when the HD path was in use.
    void print_hd_stats() const {
        if (!hd_->active()) return;
        std::printf("hd: %.2f ms per render", hd_->render_ms());
        if (hd_->ai_ms() > 0) std::printf(", AI %.2f ms per frame", hd_->ai_ms());
        if (!hd_->scene().empty()) std::printf(", widescreen scene %s", hd_->scene().c_str());
        std::printf("\n");
    }
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    void on_vblank() {
        if (args_.frames && sys_.frames() >= *args_.frames) {
            stop();
        }
        if (args_.seconds && static_cast<double>(platform::now_ns() - start_ns_) / 1e9 >= *args_.seconds) {
            stop();
        }
        measure_fps();
        if (save_) {
            if (const auto error = save_->on_frame()) {
                std::fprintf(stderr, "warning: %s\n", error->c_str());
            }
        }
        if (!poll()) {
            stop();
            return;
        }
        const std::uint32_t held = hotkeys_held();
        const std::uint32_t pressed = held & ~hotkeys_prev_;
        hotkeys_prev_ = held;
        handle_hotkeys(pressed);
        handle_touch(false);
        std::uint16_t keys = gba_keys();
        if (control_) keys = control_->keys(keys);
        sys_.set_keys(keys);

        if (cheats_.generation() != cheats_seen_) {
            sync_rom_patches();
        }
        const cheats::FrameKeys frame_keys{keys, anygba::input::has(held, Hotkey::CheatButton)};
        for (const cheats::Program* program : cheats_.active()) {
            cheats::run(*program, sys_, frame_keys);
        }

        fast_forward_held_ = anygba::input::has(held, Hotkey::FastForward) || fast_forward_latched_;
        if (effective_speed() != pacer_speed_) reset_pacer();
        update_pacing();
        const bool unthrottled = this->unthrottled();
        present(unthrottled);
        if (mapping_.normal_speed_music || mp2k_.scaled()) {
            mp2k_.update(sys_.memory(), normal_speed_music() ? audio_speed() : 100);
        }
        hd_->between_frames();
        for (auto& m : hd_->take_messages()) {
            using Session = anygba::video::hd::Session;
            const bool progress = m.starts_with(Session::kAiDownloadPrefix);
            osd_.show(std::move(m), 150, progress ? Session::kAiDownloadPrefix : "");
        }
        for (const auto& [frame, file] : args_.screenshots) {
            if (frame == sys_.frames()) screenshot(file, args_.screenshot_size);
        }
        sound_.clear();
        sys_.drain_audio(sound_);
        if (audio_ || wav_) process_audio();
        if (audio_) {
            feed_audio_and_wait(unthrottled);
        } else if (pacer_ && !unthrottled) {
            while (!between_.empty() && platform::now_ns() < between_.front().at) show_between(false);
            pacer_->wait();
        }
        show_between(true);
        if (cadence_) present_paced();
        osd_.tick();
        if (touch_ && touch_->paused()) run_touch_pause();
        poll_dialog();
        if (menu_.is_open()) {
            run_menu();
        }
        if (control_ && !stopping_) {
            control_->service_frame();
        }
        vblank_end_ns_ = platform::now_ns();
    }

    // After the run: completes the --wav file. Returns an error message.
    std::optional<std::string> finish() {
        if (!wav_) return std::nullopt;
        auto error = wav_->finish();
        wav_.reset();
        return error;
    }

    void stop() {
        stopping_ = true;
        sys_.stop();
    }

    // Agent control and the journal. Both start off unless `control_on` / `journal_on`.
    void attach_agent_features(anygba::control::Settings settings, anygba::control::Symbols symbols,
                               std::unique_ptr<anygba::journal::Journal> journal, bool control_on, bool journal_on) {
        anygba::control::Hooks hooks;
        hooks.speed = [this] { return speed_; };
        hooks.set_speed = [this](int speed) { apply_speed(speed); };
        hooks.state_dir = [this] { return state_dir_; };
        hooks.state_stem = [] { return game_file_stem(); };
        hooks.after_load = [this] {
            reset_audio();
            motion_->reset();
        };
        hooks.osd = [this](const std::string& text) { osd_.show(text); };
        hooks.cheats_changed = [this] {
            sync_rom_patches();
            save_cheats();
        };
        hooks.idle = [this](std::uint16_t& user) { return idle(user); };
        hooks.fps = [this] { return fps_; };
        hooks.pad_name = [this] { return pad_name_; };
        hooks.quit = [this] { stop(); };
        control_ = std::make_unique<anygba::control::Controller>(sys_, cheats_, std::move(hooks), std::move(settings),
                                                                 std::move(symbols), std::move(journal));
        menu_.set_settings(&menu_settings_);
        if (journal_on) control_->set_journal(true, "user");
        if (control_on) {
            if (const auto error = control_->set_control(true, "user")) {
                throw std::runtime_error(*error);
            }
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
        if (touch_) keys = static_cast<std::uint16_t>(keys | touch_->gba_keys());
        return keys;
    }

    std::uint32_t hotkeys_held() const {
        return anygba::input::hotkeys_held(mapping_, source()) | (touch_ ? touch_->hotkeys() : 0u);
    }

    void write_save() {
        if (!save_) return;
        if (const auto error = save_->flush()) {
            std::fprintf(stderr, "warning: %s\n", error->c_str());
        }
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
        if (touch_) touch_->update();
        if (script_) {
            script_->advance(host_frame_);
            const auto scripted = script_->take_gamepad_events();
            events.insert(events.end(), scripted.begin(), scripted.end());
            key_presses_ = script_->take_key_presses();
        }
        for (const auto& e : events) {
            osd_.show(std::string(e.connected ? "Gamepad connected: " : "Gamepad removed: ") + e.name);
            if (e.connected) pad_name_ = e.name;
            else if (pad_name_ == e.name) pad_name_.clear();
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
        if (anygba::input::has(pressed, Hotkey::Menu)) open_menu(false);
        if (control_ && anygba::input::has(pressed, Hotkey::AiControl)) control_->set_control(!control_->control_on(), "user");
        if (control_ && anygba::input::has(pressed, Hotkey::Journal)) control_->set_journal(!control_->journal_on(), "user");
        if (anygba::input::has(pressed, Hotkey::Stats)) toggle_stats();
        if (anygba::input::has(pressed, Hotkey::Smoothing)) {
            video_options_.smoothing = !video_options_.smoothing;
            apply_video();
            osd_.show(video_options_.smoothing ? "Smoothing on" : "Smoothing off");
        }
    }

    // The floating button's quick menu (Android): carries out its choices and tells it what
    // to show. `scan_slots` looks which state slots exist (only while the menu is open).
    void handle_touch(bool scan_slots) {
        if (!touch_) return;
        for (const player::TouchAction& a : touch_->take_actions()) {
            using player::assist::Item;
            switch (a.item) {
            case Item::SaveState: save_state(slot_); break;
            case Item::LoadState:
                if (a.slot >= 1 && a.slot <= player::assist::kSlots) {
                    slot_ = a.slot;
                    load_state(slot_);
                }
                break;
            case Item::FastForward:
                fast_forward_latched_ = !fast_forward_latched_;
                osd_.show(fast_forward_latched_ ? "Fast-forward on" : "Fast-forward off");
                break;
            case Item::FastForwardSpeed: {
                const auto& speeds = player::kFastForwardSpeeds;
                const auto at = std::find(speeds.begin(), speeds.end(), mapping_.fast_forward);
                mapping_.fast_forward = at == speeds.end() || at + 1 == speeds.end() ? speeds.front() : *(at + 1);
                save_mapping();
                osd_.show("Fast-forward " + player::format_fast_forward(mapping_.fast_forward));
                break;
            }
            case Item::Cheats: open_menu(true); break;
            case Item::Screenshot: take_screenshot(); break;
            case Item::Filter: {
                const auto effects = anygba::video::all_effects();
                const auto at = std::find(effects.begin(), effects.end(), video_options_.effect);
                video_options_.effect = at == effects.end() || at + 1 == effects.end() ? effects.front() : *(at + 1);
                apply_video();
                osd_.show("Filter: " + std::string(anygba::video::name(video_options_.effect)));
                break;
            }
            case Item::Widescreen: {
                const auto modes = anygba::video::all_widescreens();
                const auto at = std::find(modes.begin(), modes.end(), video_options_.widescreen);
                video_options_.widescreen = at == modes.end() || at + 1 == modes.end() ? modes.front() : *(at + 1);
                apply_video();
                osd_.show("Widescreen: " + std::string(anygba::video::name(video_options_.widescreen)));
                break;
            }
            case Item::GameMenu:
                if (menu_.is_open()) {
                    menu_.close();
                } else {
                    open_menu(false);
                }
                break;
            case Item::Exit:
                write_save();
                stop();
                break;
            default: break;
            }
        }
        player::TouchStatus status = touch_status_;
        status.fast_forward = fast_forward_latched_;
        status.fast_forward_speed = player::format_fast_forward(mapping_.fast_forward);
        status.slot = slot_;
        status.filter = std::string(anygba::video::name(video_options_.effect));
        status.widescreen = std::string(anygba::video::name(video_options_.widescreen));
        status.game_menu_open = menu_.is_open();
        status.language = mapping_.language;
        status.light = mapping_.light_theme;
        if (scan_slots) {
            status.saved_slots = 0;
            for (int slot = 1; slot <= player::assist::kSlots; ++slot) {
                std::error_code ec;
                const auto path = state_path(slot);
                if (path && fs::exists(*path, ec)) status.saved_slots |= static_cast<std::uint16_t>(1u << (slot - 1));
            }
        }
        if (status != touch_status_) {
            touch_status_ = status;
            touch_->set_status(status);
        }
    }

    // The game waits while the quick menu or the layout editor is open.
    void run_touch_pause() {
        while (touch_->paused() && !stopping_) {
            if (!poll()) {
                stop();
                return;
            }
            handle_touch(true);
            osd_.tick();
            if (window_) {
                if (const anygba::video::Image* hd = hd_picture()) {
                    video_->present(*hd, hd_->logical_width(), hd_->logical_height());
                } else {
                    frame_copy_ = sys_.framebuffer();
                    decorate();
                    show(frame_copy_);
                }
            }
            platform::sleep_ns(1'000'000'000ull / static_cast<unsigned>(video_options_.menu_fps));
        }
        hotkeys_prev_ = hotkeys_held();
        reset_audio();
        cadence_.reset();
    }

    // A PNG of the game picture (filters included) in <data>/screenshots, which the Android
    // activity then copies to the gallery.
    void take_screenshot() {
        const auto data = data_dir();
        if (!data) {
            osd_.show("No folder for screenshots");
            return;
        }
        const fs::path dir = *data / "screenshots";
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            osd_.show("Screenshot failed: " + ec.message());
            return;
        }
        const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count();
        const fs::path file = dir / (game_file_stem() + "-" + std::to_string(stamp) + ".png");
        const int lw = hd_->logical_width();
        screenshot(file, {lw * 4, anygba::kScreenHeight * 4});
        if (!fs::exists(file, ec)) {
            osd_.show("Screenshot failed");
            return;
        }
        osd_.show("Screenshot saved");
#if defined(__ANDROID__)
        if (!SDL_SendAndroidMessage(player::kScreenshotMessage, 0)) {
            std::fprintf(stderr, "warning: cannot publish the screenshot: %s\n", SDL_GetError());
        }
#endif
    }

    void set_speed(int speed) {
        apply_speed(speed);
        user_event("speed", anygba::control::Json(anygba::control::Json::Object{{"percent", speed}}));
    }

    void apply_speed(int speed) {
        speed_ = speed;
        reset_pacer();
        osd_.show("Speed " + std::to_string(speed_) + "%");
    }

    void user_event(std::string_view kind, anygba::control::Json data) {
        if (control_) control_->user_event(kind, std::move(data));
    }

    void sync_menu_settings() {
        if (!control_) return;
        menu_settings_.ai_control = control_->control_on();
        menu_settings_.readonly_agent = control_->readonly();
        menu_settings_.journal = control_->journal_on();
    }

    void apply_menu_settings() {
        if (!control_) return;
        if (menu_settings_.ai_control != control_->control_on()) control_->set_control(menu_settings_.ai_control, "user");
        if (menu_settings_.readonly_agent != control_->readonly()) control_->set_readonly(menu_settings_.readonly_agent, "user");
        if (menu_settings_.journal != control_->journal_on()) control_->set_journal(menu_settings_.journal, "user");
        sync_menu_settings();  // a switch that failed to turn on shows Off again
    }

    // One-second windows: the frame rate for agents, and the stats overlay's figures.
    void measure_fps() {
        const std::uint64_t now = platform::now_ns();
        if (vblank_end_ns_) work_ns_ += now - vblank_end_ns_;
        if (fps_frames_++ == 0) {
            fps_start_ns_ = now;
            start_stats_window();
        } else if (now - fps_start_ns_ >= 1'000'000'000ull) {
            const double seconds = static_cast<double>(now - fps_start_ns_) / 1e9;
            fps_ = static_cast<double>(fps_frames_ - 1) / seconds;
            if (mapping_.stats) update_stats(seconds, fps_frames_ - 1);
            fps_frames_ = 1;
            fps_start_ns_ = now;
            start_stats_window();
        }
    }

    void start_stats_window() {
        window_cycles_ = sys_.now();
        window_interpreted_ = sys_.interpreted_cycles();
        presents_ = 0;
        work_ns_ = 0;
    }

    void update_stats(double seconds, std::uint64_t frames) {
        player::Stats s;
        s.host_fps = static_cast<double>(presents_) / seconds;
        s.gba_fps = fps_;
        s.fast_forward = fast_forward_held_;
        s.target_speed = unthrottled() ? player::kUnlimited : effective_speed();
        s.frame_ms = frames ? static_cast<double>(work_ns_) / 1e6 / static_cast<double>(frames) : 0;
        if (audio_) {
            s.audio_fill = 100.0 * static_cast<double>(audio_->queued_frames()) / (2.0 * kSampleRate / 60);
        }
        s.underruns = sys_.fifo_underruns();
        const std::uint64_t cycles = sys_.now();
        const std::uint64_t interpreted = sys_.interpreted_cycles();
        if (cycles > window_cycles_ && interpreted >= window_interpreted_) {
            s.interpreted = std::min(100.0, 100.0 * static_cast<double>(interpreted - window_interpreted_) /
                                                static_cast<double>(cycles - window_cycles_));
        }
        s.filter = anygba::video::filter_spec(video_options_);
        if (window_) {
            const auto [w, h] = window_->render_size();
            s.output = std::to_string(w) + "x" + std::to_string(h);
        } else {
            s.output = "headless";
        }
        if (window_) {
            char display[96];
            const double hz = monitor_hz();
            std::snprintf(display, sizeof display, "%.0f Hz %s, %s", hz, video_options_.refresh ? "set" : "auto",
                          cadence_ ? (vsync_set_ ? "vsync" : "timer") : video_options_.vrr ? "VRR" : "free");
            s.display = display;
            if (cadence_ && sync_factor_ != 1.0) {
                std::snprintf(display, sizeof display, ", synced %.2f fps", cadence_->game_hz());
                s.display += display;
            }
        } else {
            s.display = "none";
        }
        s.cheats = cheats_.active().size();
        s.save = anygba::save::name(sys_.backup().type());
        s.rtc = sys_.has_rtc();
        stats_lines_ = player::stats_lines(s);
        if (args_.headless) {
            for (const auto& l : stats_lines_) std::printf("stats| %s\n", l.c_str());
            std::fflush(stdout);
        }
    }

    void toggle_stats() {
        mapping_.stats = !mapping_.stats;
        stats_lines_.clear();  // filled at the end of the next one-second window
        osd_.show(mapping_.stats ? "Stats on" : "Stats off");
        save_mapping();
    }

    // The overlays drawn over the game picture in frame_copy_.
    void decorate() {
        if (mapping_.stats) player::draw_stats(frame_copy_, stats_lines_);
        osd_.draw(frame_copy_);
        draw_indicator();
    }

    // One host frame while an agent keeps the game paused.
    bool idle(std::uint16_t& user) {
        user = 0;
        if (!window_) return true;
        if (!platform::pump_events(*input_)) return false;
        input_->take_key_presses();
        for (const auto& e : input_->take_gamepad_events()) {
            if (e.connected) pad_name_ = e.name;
            osd_.show(std::string(e.connected ? "Gamepad connected: " : "Gamepad removed: ") + e.name);
        }
        const std::uint32_t held = anygba::input::hotkeys_held(mapping_, *device_);
        const std::uint32_t pressed = held & ~hotkeys_prev_;
        hotkeys_prev_ = held;
        if (anygba::input::has(pressed, Hotkey::AiControl)) control_->set_control(!control_->control_on(), "user");
        if (anygba::input::has(pressed, Hotkey::Journal)) control_->set_journal(!control_->journal_on(), "user");
        if (anygba::input::has(pressed, Hotkey::Stats)) toggle_stats();
        user = anygba::input::gba_keys(mapping_, *device_);
        frame_copy_ = sys_.framebuffer();
        decorate();
        show(frame_copy_);
        osd_.tick();
        return true;
    }

    void draw_indicator() {
        const std::string text = control_ ? control_->indicator() : std::string();
        if (text.empty()) return;
        const int width = static_cast<int>(text.size()) * player::kCellWidth;
        player::shade(frame_copy_, anygba::kScreenWidth - width - 4, 0, width + 4, player::kCellHeight + 1);
        player::draw_text(frame_copy_, anygba::kScreenWidth - width - 2, 1, text, player::color::kCyan);
    }

    // The speed the game runs at now: the fast-forward rate while its hotkey is held.
    int effective_speed() const {
        return fast_forward_held_ && mapping_.fast_forward != player::kUnlimited ? mapping_.fast_forward : speed_;
    }

    bool unthrottled() const {
        return !args_.pace || (fast_forward_held_ && mapping_.fast_forward == player::kUnlimited);
    }

    void reset_pacer() {
        pacer_.reset();
        pacer_speed_ = effective_speed();
        if (args_.pace && !audio_) {
            pacer_ = std::make_unique<platform::FramePacer>(anygba::kFramesPerSecond * pacer_speed_ / 100.0);
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
                    write_thumbnail(path);
                    osd_.show("Saved state " + std::to_string(slot));
                    user_event("state_save", anygba::control::Json(anygba::control::Json::Object{{"slot", slot}}));
                }
            } catch (const std::exception& e) {
                osd_.show("Save state " + std::to_string(slot) + " failed: " + e.what());
            }
        });
    }

    // The picture of the moment, next to the state as <state>.png, for the Save states page.
    void write_thumbnail(const fs::path& state) {
        anygba::video::Image image;
        image.resize(anygba::kScreenWidth, anygba::kScreenHeight);
        const auto& frame = sys_.framebuffer();
        for (std::size_t i = 0; i < image.pixels.size(); ++i) {
            image.pixels[i] = anygba::video::hd::expand(static_cast<std::uint16_t>(frame[i] & 0x7FFF));
        }
        fs::path file = state;
        file += ".png";
        if (const auto error = anygba::video::hd::save_png(file, image)) {
            std::fprintf(stderr, "warning: %s\n", error->c_str());
        }
    }

    void delete_state(int slot) {
        const auto path = state_path(slot);
        if (!path) return;
        std::error_code ec;
        fs::path thumb = *path;
        thumb += ".png";
        fs::remove(thumb, ec);
        if (!fs::remove(*path, ec) || ec) {
            osd_.show("Cannot delete state " + std::to_string(slot) + (ec ? ": " + ec.message() : std::string()));
            return;
        }
        osd_.show("Deleted state " + std::to_string(slot));
        refresh_slots();
    }

    // What the Save states page shows: which slots hold a state, when it was saved, and its
    // thumbnail.
    void refresh_slots() {
        std::array<player::SlotInfo, player::kStateSlots> slots{};
        for (int slot = 1; slot <= player::kStateSlots; ++slot) {
            const auto path = state_path(slot);
            std::error_code ec;
            if (!path || !fs::exists(*path, ec)) continue;
            player::SlotInfo& info = slots[static_cast<std::size_t>(slot - 1)];
            info.used = true;
            const auto written = fs::last_write_time(*path, ec);
            if (!ec) {
                // file_clock to system_clock without clock_cast, which not every C++ library has.
                const auto at = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    written - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                const std::time_t t = std::chrono::system_clock::to_time_t(at);
                std::tm tm{};
#if defined(_WIN32)
                localtime_s(&tm, &t);
#else
                localtime_r(&t, &tm);
#endif
                char text[32];
                std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &tm);
                info.when = text;
            }
            fs::path thumb = *path;
            thumb += ".png";
            if (fs::exists(thumb, ec)) {
                std::string error;
                if (auto png = anygba::video::hd::load_png(thumb, &error)) {
                    info.thumb = std::move(png->pixels);
                    info.thumb_width = png->width;
                    info.thumb_height = png->height;
                }
            }
        }
        menu_.set_slots(slots);
    }

    void open_menu(bool cheats) {
        sync_menu_settings();
        refresh_slots();
        if (cheats) {
            menu_.open_cheats();
        } else {
            menu_.open();
        }
        menu_trace_.clear();
        osd_seen_ = osd_.count();
        if (input_) input_->take_pointer_events();  // clicks made in the game are not for the menu
        if (window_) {
            menu_view_.open();
            menu_backdrop_stale_ = true;
            preview_stale_ = true;
        }
    }

    anyui::ViewOptions menu_look() const {
        anyui::ViewOptions o;
        o.ui_scale = static_cast<float>(mapping_.ui_scale) / 100.f;
        o.dark = !mapping_.light_theme;
        o.reduce_motion = mapping_.reduce_motion;
        o.touch = touch_ != nullptr;
        return o;
    }

    // One picture of the menu over the frosted game picture, at the window's resolution.
    void draw_menu(float dt) {
        const auto [w, h] = window_->output_pixels();
        if (w <= 0 || h <= 0) return;
        const anyui::ViewOptions look = menu_look();
        if (menu_backdrop_stale_ || !menu_view_.background_fits(w, h, look)) {
            menu_backdrop_stale_ = false;
            shot_pipeline_.set_options(video_options_);
            const anygba::video::Image* hd = hd_->picture();
            const int lw = hd ? hd_->logical_width() : anygba::kScreenWidth;
            try {
                const anygba::video::Image& image = hd ? shot_pipeline_.render(*hd, lw, anygba::kScreenHeight, w, h)
                                                       : shot_pipeline_.render(sys_.framebuffer(), lw, anygba::kScreenHeight, w, h);
                menu_view_.set_background(image.pixels.data(), w, h, look);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "warning: menu backdrop: %s\n", e.what());
                menu_view_.set_background(nullptr, w, h, look);
            }
        }
        anyui::Frame frame = menu_.view();
        if (frame.page.preview.shown) {
            update_preview(w, h);
            anyui::Preview& pv = frame.page.preview;
            pv.before = {preview_before_.pixels.data(), preview_before_.width, preview_before_.height};
            pv.after = {preview_after_.pixels.data(), preview_after_.width, preview_after_.height};
            pv.version = preview_version_;
            if (!preview_shows_hd_) pv.note = player::menu_strings().tr("preview.resume");
        }
        menu_view_.render(menu_canvas_, frame, look, dt);
        window_->present_ui(menu_canvas_.pixels(), menu_canvas_.width(), menu_canvas_.height());
    }

    // The Video preview's pictures, at the size the game takes in a w x h window so the loupe
    // shows real pixels: before is the plain 1x frame (nearest), after the whole chain with the
    // current settings. Redone after a Video change; an HD change draws the paused frame again.
    void update_preview(int w, int h) {
        if (!preview_stale_ && preview_size_ == std::pair{w, h}) return;
        const bool hd_changed = preview_stale_;
        preview_stale_ = false;
        preview_size_ = {w, h};
        ++preview_version_;
        const bool hd_on = hd_->active();
        bool hd_drawn = !hd_on;
        if (hd_on && hd_changed) hd_drawn = hd_->render_again(sys_);
        else if (hd_on) hd_drawn = preview_hd_drawn_;
        preview_hd_drawn_ = hd_drawn;
        // AI pictures come from a worker some time later.
        preview_shows_hd_ = hd_drawn && video_options_.ai == anygba::video::Ai::Off;
        const anygba::video::Image* hd = hd_drawn ? hd_->picture() : nullptr;
        const int lw = hd ? hd_->logical_width() : anygba::kScreenWidth;
        try {
            const anygba::video::Rect plain = anygba::video::fit(w, h, anygba::kScreenWidth, anygba::kScreenHeight, false);
            preview_before_ = preview_plain_.render(sys_.framebuffer(), anygba::kScreenWidth, anygba::kScreenHeight, plain.w, plain.h);
            const anygba::video::Rect area = anygba::video::fit(w, h, lw, anygba::kScreenHeight, false);
            preview_pipeline_.set_options(video_options_);
            preview_after_ = hd ? preview_pipeline_.render(*hd, lw, anygba::kScreenHeight, area.w, area.h)
                                : preview_pipeline_.render(sys_.framebuffer(), lw, anygba::kScreenHeight, area.w, area.h);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "warning: video preview: %s\n", e.what());
            preview_before_ = {};
            preview_after_ = {};
        }
    }

    // Mouse and touch in the menu: hover, click or tap, wheel or drag to scroll.
    std::vector<player::MenuAction> menu_pointer() {
        std::vector<player::MenuAction> out;
        if (!input_ || !window_) return out;
        const auto [w, h] = window_->output_pixels();
        for (const platform::PointerEvent& e : input_->take_pointer_events()) {
            const float x = e.x * static_cast<float>(w), y = e.y * static_cast<float>(h);
            using Kind = platform::PointerEvent::Kind;
            using Phase = anyui::MenuView::PointerPhase;
            if (e.kind != Kind::Wheel && (!e.touch || !drag_finger_ || *drag_finger_ != e.finger)) {
                const Phase phase = e.kind == Kind::Down ? Phase::Down : e.kind == Kind::Up ? Phase::Up : Phase::Move;
                if (menu_view_.preview_pointer(menu_.preview_state(), phase, x, y, e.touch)) continue;
            }
            if (e.kind == Kind::Wheel) {
                menu_view_.wheel(e.wheel);
            } else if (e.kind == Kind::Move) {
                if (e.touch && drag_finger_ == e.finger) {
                    const float dy = drag_y_ - y;
                    if (std::abs(y - press_y_) > 12) dragged_ = true;
                    if (dragged_) menu_view_.wheel(dy / std::max(1.f, 40.f * std::min(static_cast<float>(w) / 960.f, static_cast<float>(h) / 540.f) * 1.5f));
                    drag_y_ = y;
                } else if (!e.touch) {
                    menu_view_.hover(x, y);
                }
            } else if (e.kind == Kind::Down) {
                if (e.touch) {
                    drag_finger_ = e.finger;
                    drag_y_ = press_y_ = y;
                    dragged_ = false;
                } else {
                    const auto a = menu_.click(menu_view_.hit(x, y));
                    out.insert(out.end(), a.begin(), a.end());
                }
            } else if (e.kind == Kind::Up && e.touch && drag_finger_ == e.finger) {
                drag_finger_.reset();
                if (!dragged_ && e.x >= 0) {
                    const auto a = menu_.click(menu_view_.hit(x, y));
                    out.insert(out.end(), a.begin(), a.end());
                }
            }
        }
        return out;
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
                reset_audio();
                osd_.show("Loaded state " + std::to_string(slot));
                user_event("state_load", anygba::control::Json(anygba::control::Json::Object{{"slot", slot}}));
            } catch (const std::exception& e) {
                osd_.show("Load state " + std::to_string(slot) + " failed: " + e.what());
            }
        });
    }

    void sync_rom_patches() {
        cheats_seen_ = cheats_.generation();
        // Widescreen rule patches first, so a cheat on the same address wins.
        auto patches = hd_ ? hd_->rom_patches() : std::map<std::uint32_t, std::uint16_t>{};
        for (const auto& [addr, value] : cheats::rom_patches(cheats_.active())) patches[addr] = value;
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

    void choose_save_folder() {
        if (!window_) {
            osd_.show("No folder picker without a window; set SaveFolder in the input file");
            return;
        }
        const std::string start = state_dir_ ? path_utf8(*state_dir_) : std::string();
        if (dialog_.show(player::FileDialog::Kind::Folder, start)) dialog_for_ = DialogFor::SaveFolder;
    }

    // Acts on a picker that has closed.
    void poll_dialog() {
        if (dialog_for_ == DialogFor::None) return;
        const auto answer = dialog_.take();
        if (!answer) return;
        const DialogFor what = dialog_for_;
        dialog_for_ = DialogFor::None;
        if (answer->empty()) return;  // cancelled
        if (what == DialogFor::SaveFolder) set_save_folder(*answer);
        if (what == DialogFor::ExportSave) export_save(utf8_path(answer->c_str()));
        if (what == DialogFor::ImportSave) import_save(utf8_path(answer->c_str()));  // shows its own errors
    }

    // "<ROM name>.sav" next to the ROM, as VisualBoyAdvance and mGBA name it; for an embedded
    // ROM "<title>.sav" in the save folder.
    std::optional<fs::path> default_sav() const {
        if (!args_.rom.empty()) {
            fs::path rom = fs::path(args_.rom);
            return rom.replace_extension(".sav");
        }
        if (!state_dir_) return std::nullopt;
        std::string name;
        for (const char* p = anygba_game_info.title; *p; ++p) {
            const char ch = *p;
            name += std::string_view("<>:\"/\\|?*").find(ch) == std::string_view::npos && ch >= ' ' ? ch : '_';
        }
        return *state_dir_ / ((name.empty() ? std::string("game") : name) + ".sav");
    }

    void export_from_menu() {
        const auto target = default_sav();
        if (window_) {
            if (dialog_.show(player::FileDialog::Kind::Save, target ? path_utf8(*target) : std::string(), "Game saves",
                             "sav")) {
                dialog_for_ = DialogFor::ExportSave;
            }
            return;
        }
        if (!target) {
            osd_.show("No folder to export to; pass --save-dir");
            return;
        }
        export_save(*target);
    }

    void import_from_menu() {
        const auto source = default_sav();
        if (window_) {
            if (dialog_.show(player::FileDialog::Kind::Open, source ? path_utf8(*source) : std::string(), "Game saves",
                             "sav")) {
                dialog_for_ = DialogFor::ImportSave;
            }
            return;
        }
        if (!source) {
            osd_.show("No .sav to import; pass --save-dir");
            return;
        }
        import_save(*source);
    }

    void export_save(const fs::path& target) {
        const auto& data = sys_.backup().data();
        if (data.empty()) {
            osd_.show(sys_.backup().type() == anygba::save::Type::None ? "This game has no save chip"
                                                                       : "Nothing saved yet");
            return;
        }
        if (const auto error = anygba::save::replace_file(target, data, false)) {
            osd_.show("Export failed: " + *error);
        } else {
            osd_.show("Exported " + player::to_ascii(path_utf8(target.filename())));
        }
    }

public:
    // Loads a .sav from another emulator into the save chip. `restart`: then resets the game at
    // the next safe point so it reads it (from the menu); else the game has not started yet.
    // The current save file is copied to <save>.before-import-<time> first. Returns an error.
    std::optional<std::string> import_save(const fs::path& source, bool restart = true) {
        std::string error;
        std::optional<std::vector<std::uint8_t>> bytes;
        try {
            bytes = anygba::save::fit_import(sys_.backup().type(), read_bytes(source), &error);
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!bytes) {
            osd_.show("Import failed: " + error);
            return path_utf8(source) + ": " + error;
        }
        if (save_) {
            std::error_code ec;
            if (fs::exists(save_->path(), ec)) {
                fs::path keep = save_->path();
                keep += ".before-import-" + timestamp();
                if (const auto copy_error = anygba::save::replace_file(keep, read_bytes(save_->path()), false)) {
                    osd_.show("Import cancelled: " + *copy_error);
                    return copy_error;
                }
            }
        }
        const std::string name = player::to_ascii(path_utf8(source.filename()));
        if (!restart) {
            try {
                sys_.backup().load(std::move(*bytes));
            } catch (const std::exception& e) {
                osd_.show(std::string("Import failed: ") + e.what());
                return path_utf8(source) + ": " + e.what();
            }
            if (save_) {
                if (const auto write_error = save_->write_now()) return write_error;
            }
            osd_.show("Imported " + name);
            return std::nullopt;
        }
        auto shared = std::make_shared<std::vector<std::uint8_t>>(std::move(*bytes));
        sys_.request_safe_point([this, shared, name](anygba::System& s) {
            try {
                s.backup().load(std::move(*shared));
            } catch (const std::exception& e) {
                osd_.show(std::string("Import failed: ") + e.what());
                return;
            }
            if (save_) {
                if (const auto write_error = save_->write_now()) osd_.show(*write_error);
            }
            s.reset();
            reset_audio();
            osd_.show("Imported " + name + "; game restarted");
        });
        menu_.close();
        return std::nullopt;
    }

private:
    static std::string timestamp() {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char text[32];
        std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &tm);
        return text;
    }

    void set_save_folder(const std::string& folder) {
        mapping_.save_folder = folder;
        save_mapping();
        osd_.show("Save folder: " + player::to_ascii(folder) + " (from the next start)");
    }

    void save_mapping() {
        if (!mapping_path_) {
            osd_.show("No folder for anygba-input.ini");
            return;
        }
        // A --ff-speed value is for this run only, unless the menu changed it since.
        anygba::input::Mapping saved = mapping_;
        if (args_.fast_forward && mapping_.fast_forward == *args_.fast_forward) saved.fast_forward = file_fast_forward_;
        if (const auto error = anygba::input::save_mapping(saved, *mapping_path_)) {
            osd_.show(*error);
        }
    }

    // The game is paused: the menu runs one host frame at a time until it closes.
    void run_menu() {
        std::uint32_t pad_prev = source().pad().buttons;
        const std::uint64_t script_end = script_ ? script_->last_frame() : 0;
        platform::FramePacer menu_pacer(video_options_.menu_fps);
        while (menu_.is_open()) {
            if (!poll()) {
                stop();
                return;
            }
            const std::uint32_t held = hotkeys_held();
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
            const auto stick = [&](platform::Axis a) {
                constexpr float kDeadZone = 8000;
                const float v = static_cast<float>(pad.axis(a));
                return std::abs(v) < kDeadZone ? 0.f : (v - std::copysign(kDeadZone, v)) / (32767.f - kDeadZone);
            };
            in.stick_x = stick(platform::Axis::RightX);
            in.stick_y = stick(platform::Axis::RightY);
            // Only a gamepad chord closes the menu through the hotkey; Escape arrives as a key.
            in.menu_hotkey = anygba::input::has(pressed_hotkeys, Hotkey::Menu) &&
                             std::find(in.keys.begin(), in.keys.end(), "Escape") == in.keys.end();

            std::vector<player::MenuAction> actions = menu_.update(in);
            const auto pointer = menu_pointer();
            actions.insert(actions.end(), pointer.begin(), pointer.end());
            for (const player::MenuAction action : actions) {
                switch (action) {
                case player::MenuAction::SaveState: save_state(slot_); break;
                case player::MenuAction::LoadState: load_state(slot_); break;
                case player::MenuAction::SpeedChanged: set_speed(speed_); break;
                case player::MenuAction::CheatsChanged:
                    sync_rom_patches();
                    save_cheats();
                    user_event("cheats", active_cheats_json());
                    break;
                case player::MenuAction::MappingChanged: save_mapping(); break;
                case player::MenuAction::VideoChanged: apply_video(); break;
                case player::MenuAction::SettingsChanged: apply_menu_settings(); break;
                case player::MenuAction::ChooseSaveFolder: choose_save_folder(); break;
                case player::MenuAction::ExportSave: export_from_menu(); break;
                case player::MenuAction::ImportSave: import_from_menu(); break;
                case player::MenuAction::DeleteState: delete_state(slot_); break;
                case player::MenuAction::Reset:
                    sys_.request_safe_point([this](anygba::System& s) {
                        s.reset();
                        reset_audio();
                        osd_.show("Game reset");
                    });
                    break;
                case player::MenuAction::Exit:
                    write_save();
                    stop();
                    break;
                case player::MenuAction::StatsChanged:
                    stats_lines_.clear();  // filled at the end of the next one-second window
                    break;
                case player::MenuAction::Close: break;
                }
                if (action == player::MenuAction::VideoChanged) menu_backdrop_stale_ = preview_stale_ = true;
            }
            handle_touch(false);
            poll_dialog();
            if (osd_.count() != osd_seen_) {
                osd_seen_ = osd_.count();
                menu_.say(osd_.last());
            }
            trace_menu();
            osd_.tick();

            if (window_ && menu_.is_open()) {
                const std::uint64_t now = platform::now_ns();
                const float dt = menu_frame_ns_ ? std::min(0.1f, static_cast<float>(now - menu_frame_ns_) / 1e9f) : 1.f / 60;
                menu_frame_ns_ = now;
                draw_menu(dt);
                menu_pacer.wait();
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
        if (window_ && !stopping_) {
            // The menu fades out over the game picture before the game goes on.
            menu_view_.close();
            for (int i = 0; i < 30 && !menu_view_.closed(); ++i) {
                draw_menu(1.f / 60);
                platform::sleep_ns(16'000'000);
            }
        }
        menu_frame_ns_ = 0;
        if (input_) input_->take_pointer_events();
        hotkeys_prev_ = hotkeys_held();
        reset_audio();
        cadence_.reset();
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

    void present(bool unthrottled) {
        const std::uint64_t now = platform::now_ns();
        if (smoothing_wanted()) motion_->end_frame();
        if (cadence_) return;  // present_paced shows it on the display's vblanks
        if (smoothing_wanted()) {
            if (!unthrottled && effective_speed() == 100 && !hd_->picture() && plan_between(now)) return;
        }
        if ((unthrottled || effective_speed() > 100) && now - last_present_ns_ < 15'000'000) {
            return;  // fast play: show at most about 60 pictures a second
        }
        if (video_options_.fps_cap && now - last_present_ns_ < 950'000'000ull / static_cast<unsigned>(video_options_.fps_cap)) {
            return;
        }
        last_present_ns_ = now;
        ++presents_;  // counted headless too, as the pictures a window would show
        if (!window_) return;
        if (touch_) touch_->set_picture_width(hd_->logical_width());
        if (const anygba::video::Image* hd = hd_picture()) {
            video_->present(*hd, hd_->logical_width(), hd_->logical_height());
            return;
        }
        if (osd_.empty() && (!control_ || control_->indicator().empty()) && !mapping_.stats) {
            show(sys_.framebuffer());
            return;
        }
        frame_copy_ = sys_.framebuffer();
        decorate();
        show(frame_copy_);
    }

    bool smoothing_wanted() const { return video_options_.smoothing && window_; }

    // The monitor's refresh (checked once a second, so moving the window to another monitor
    // is noticed), or the rate set in the Video page.
    double monitor_hz() {
        const std::uint64_t now = platform::now_ns();
        if (window_ && (refresh_checked_ns_ == 0 || now - refresh_checked_ns_ > 1'000'000'000ull)) {
            refresh_checked_ns_ = now;
            detected_hz_ = window_->refresh_rate();
        }
        if (video_options_.refresh) return video_options_.refresh;
        return detected_hz_ > 0 ? detected_hz_ : 60.0;
    }

    // Frames are paced by the display (cadence_ set) at 100% speed in a window, unless the
    // monitor has variable refresh: then each frame is presented when ready (audio-led).
    bool display_paced() const {
        return window_ && args_.pace && !unthrottled() && effective_speed() == 100 && !video_options_.vrr;
    }

    void update_pacing() {
        if (!window_) return;
        const double hz = monitor_hz();
        const bool paced = display_paced();
        const int interval = player::vsync_interval(hz, video_options_.fps_cap);
        // VSync only keeps time when the rate pacing assumes is the monitor's real one.
        int vsync = paced && player::same_rate(hz, detected_hz_) ? interval : 0;
        if (vsync != vsync_set_) {
            if (!window_->set_vsync(vsync)) {
                vsync = 0;
                window_->set_vsync(0);
            }
            vsync_set_ = vsync;
        }
        if (!paced) {
            cadence_.reset();
            sync_factor_ = 1.0;
            audio_ratio_ = 1.0;
            return;
        }
        const double shown = hz / interval;
        const double game = video_options_.sync_to_monitor ? player::synced_game_hz(anygba::kFramesPerSecond, shown)
                                                           : anygba::kFramesPerSecond;
        if (!cadence_ || cadence_->game_hz() != game || cadence_->display_hz() != shown) {
            cadence_.emplace(game, shown);
            cad_frame_ = cad_vblank_ = 0;
            cad_base_ns_ = 0;
        }
        sync_factor_ = game / anygba::kFramesPerSecond;
    }

    // Shows the display vblanks that fall between the previous frame's time and this one's:
    // this frame (or, with smoothing, pictures between the two). Each present waits for its
    // vblank; without working vsync a timer keeps the same schedule.
    void present_paced() {
        ++cad_frame_;
        const std::uint64_t until = cadence_->first_vblank(cad_frame_);
        const double period = 1e9 / cadence_->display_hz();
        const bool smooth = smoothing_wanted() && motion_->smooth() && !hd_->picture() && !prev_frame_.empty();
        if (smooth) between_vram_ = sys_.memory().vram;
        const auto due = [&](std::uint64_t vblank) {
            return cad_base_ns_ + static_cast<std::uint64_t>(period * static_cast<double>(vblank));
        };
        while (cad_vblank_ < until) {
            std::uint64_t now = platform::now_ns();
            if (cad_base_ns_ == 0 || now > due(cad_vblank_) + 250'000'000ull) {
                // Start, or a long stall (window dragged): restart the schedule from now.
                cad_base_ns_ = now - static_cast<std::uint64_t>(period * static_cast<double>(cad_vblank_));
            }
            if (cad_vblank_ + 1 < until && now > due(cad_vblank_ + 1)) {
                ++cad_vblank_;  // this vblank has passed: skip its picture, keep the game's speed
                continue;
            }
            const double t = cadence_->position(cad_vblank_) - static_cast<double>(cad_frame_ - 1);
            if (const anygba::video::Image* hd = hd_picture()) {
                video_->present(*hd, hd_->logical_width(), hd_->logical_height());
            } else {
                if (smooth && t <= 0.0) {
                    std::copy(prev_frame_.begin(), prev_frame_.end(), frame_copy_.begin());
                } else if (smooth && t < 1.0) {
                    frame_copy_ = motion_->render(t, between_vram_);
                } else {
                    frame_copy_ = sys_.framebuffer();
                }
                decorate();
                show(frame_copy_);
            }
            ++presents_;
            ++cad_vblank_;
            now = platform::now_ns();
            const std::uint64_t next = due(cad_vblank_);
            if (now < next && (vsync_set_ == 0 || static_cast<double>(next - now) > period / 2)) {
                platform::sleep_ns(next - now);  // no vsync, or the present did not wait for it
            } else if (vsync_set_ != 0 && static_cast<double>(now) < static_cast<double>(next) + period / 2) {
                // VSync keeps the time: follow the display's clock rather than the timer's.
                cad_base_ns_ = now - static_cast<std::uint64_t>(period * static_cast<double>(cad_vblank_));
            }
        }
        prev_frame_.assign(sys_.framebuffer().begin(), sys_.framebuffer().end());
        last_present_ns_ = platform::now_ns();
    }

    // Pictures this many times the GBA frame rate fit the display (1: no room for any).
    int pictures_per_frame() {
        double hz = monitor_hz();
        if (video_options_.fps_cap) hz = std::min(hz, static_cast<double>(video_options_.fps_cap));
        return std::max(1, static_cast<int>(hz / anygba::kFramesPerSecond + 0.1));
    }

    // Queues the pictures between the previous frame and this one, spread over one GBA
    // frame from now and ending with the real frame. False when there is nothing to smooth.
    bool plan_between(std::uint64_t now) {
        between_.clear();
        const int n = pictures_per_frame();
        if (n < 2 || !motion_->smooth()) return false;
        const double period = 1e9 / anygba::kFramesPerSecond;
        for (int k = 1; k <= n; ++k) {
            between_.push_back({now + static_cast<std::uint64_t>(period * (k - 1) / n), static_cast<double>(k) / n});
        }
        between_vram_ = sys_.memory().vram;
        between_frame_.assign(sys_.framebuffer().begin(), sys_.framebuffer().end());
        show_between(false);
        return true;
    }

    // Shows the queued pictures that are due (all of them when `all`).
    void show_between(bool all) {
        while (!between_.empty() && (all || platform::now_ns() >= between_.front().at)) {
            const Between b = between_.front();
            between_.erase(between_.begin());
            if (all && !between_.empty()) continue;  // late: skip straight to the real frame
            ++presents_;
            if (b.t >= 1.0) {
                frame_copy_ = {};
                std::copy(between_frame_.begin(), between_frame_.end(), frame_copy_.begin());
            } else {
                frame_copy_ = motion_->render(b.t, between_vram_);
            }
            decorate();
            show(frame_copy_);
        }
    }

    void show(std::span<const std::uint16_t> frame) {
        video_->present(frame, anygba::kScreenWidth, anygba::kScreenHeight);
    }

    anygba::video::hd::SessionSetup hd_setup() const {
        anygba::video::hd::SessionSetup s;
        const auto& rom = sys_.memory().rom;
        for (std::size_t i = 0xAC; i < 0xB0; ++i) {
            const char ch = static_cast<char>(rom[i]);
            s.game_code += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
        }
        s.title = anygba_game_info.title;
        for (const auto& dir : {program_dir(), data_dir()}) {
            if (dir) s.search_dirs.push_back(*dir);
        }
        s.pack_dir = args_.hd_pack;
        s.dump_dir = args_.hd_dump;
        s.data_dir = data_dir();
        s.dump_filter = args_.dump_filter;
        s.ai_device = args_.ai_device;
        return s;
    }

    // The HD picture with the on-screen messages on top (drawn at 1x, then pasted over the
    // HD pixels they cover), or null when the HD path is off.
    const anygba::video::Image* hd_picture() {
        const anygba::video::Image* pic = hd_->picture();
        if (!pic || osd_.empty()) return pic;
        frame_copy_ = sys_.framebuffer();
        osd_.draw(frame_copy_);
        hd_copy_ = *pic;
        const auto& before = sys_.framebuffer();
        const int scale = hd_copy_.height / anygba::kScreenHeight;
        const int left = (hd_->logical_width() - anygba::kScreenWidth) / 2 * scale;
        for (int y = 0; y < anygba::kScreenHeight; ++y) {
            for (int x = 0; x < anygba::kScreenWidth; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * anygba::kScreenWidth + static_cast<std::size_t>(x);
                if (frame_copy_[i] == before[i]) continue;
                const std::uint32_t c = anygba::video::hd::expand(static_cast<std::uint16_t>(frame_copy_[i] & 0x7FFF));
                for (int j = 0; j < scale; ++j) std::fill_n(hd_copy_.row(y * scale + j) + left + x * scale, scale, c);
            }
        }
        return &hd_copy_;
    }

    // --screenshot: what a window would show (filters included), as a PNG.
    void screenshot(const fs::path& file, std::pair<int, int> size) {
        const anygba::video::Image* hd = hd_picture();
        shot_pipeline_.set_options(video_options_);
        const int lw = hd ? hd_->logical_width() : anygba::kScreenWidth;
        int w = size.first, h = size.second;
        if (w == 0) {
            w = hd ? hd->width : lw;
            h = hd ? hd->height : anygba::kScreenHeight;
        }
        try {
            const anygba::video::Image& image = hd ? shot_pipeline_.render(*hd, lw, anygba::kScreenHeight, w, h)
                                                   : shot_pipeline_.render(sys_.framebuffer(), lw, anygba::kScreenHeight, w, h);
            if (const auto error = anygba::video::hd::save_png(file, image)) {
                std::fprintf(stderr, "error: %s\n", error->c_str());
            } else {
                std::printf("screenshot: frame %llu -> %s (%dx%d)\n", static_cast<unsigned long long>(sys_.frames()),
                            file.string().c_str(), w, h);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: screenshot: %s\n", e.what());
        }
    }

    std::optional<fs::path> video_file() const {
        if (!state_dir_) return std::nullopt;
        return *state_dir_ / (game_file_stem() + ".video");
    }

    // Menu changes apply at once and are kept for the next run of this game.
    void apply_video() {
        hd_->set_options(video_options_);
        sync_rom_patches();  // widescreen rules may patch the ROM
        if (video_) {
            try {
                video_->set_options(video_options_);
            } catch (const std::exception& e) {
                osd_.show(e.what());
            }
        }
        if (const auto file = video_file()) {
            if (const auto error = anygba::video::save_settings(*file, video_options_)) osd_.show(*error);
        }
    }

    anygba::control::Json active_cheats_json() const {
        anygba::control::Json names = anygba::control::Json::array();
        for (const auto& c : cheats_.cheats()) {
            if (c.enabled) names.push_back(c.name);
        }
        return anygba::control::Json(anygba::control::Json::Object{{"active", std::move(names)}});
    }

    // The speed the sound is squeezed by: the game speed, also in unpaced runs (so --wav
    // records what a paced run plays), and 400% for unlimited fast-forward.
    int audio_speed() const {
        return fast_forward_held_ && mapping_.fast_forward == player::kUnlimited ? 400 : effective_speed();
    }

    // Normal-speed music: MP2K games get their sequencer slowed by the speed, and the sound is
    // time-stretched instead of resampled, so it keeps its pitch.
    bool normal_speed_music() const {
        return mapping_.normal_speed_music && audio_speed() > 100 &&
               !(fast_forward_held_ && mapping_.fast_forward == player::kUnlimited);
    }

    // Squeezes this frame's sound (sound_) into squeezed_, for the device and --wav.
    void process_audio() {
        squeezed_.clear();
        const bool stretch = normal_speed_music();
        if (stretch != stretching_) {
            reset_audio();
            stretching_ = stretch;
        }
        if (stretch) {
            stretch_.process(sound_, audio_speed() / 100.0 * audio_ratio_, squeezed_);
        } else {
            resampler_.process(sound_, audio_speed() / 100.0 * audio_ratio_, squeezed_);
        }
        if (wav_) wav_->write(squeezed_);
    }

    // The Volume and Mute settings, on what goes to the sound card only (not --wav).
    void apply_volume() {
        const int volume = mapping_.mute ? 0 : mapping_.volume;
        if (volume >= 100) return;
        for (auto& sample : squeezed_) sample = static_cast<std::int16_t>(sample * volume / 100);
    }

    void reset_audio() {
        resampler_.reset();
        stretch_.reset();
    }

    // Audio-led pacing: the device consumes 32768 frames per second of sound (SDL resamples
    // to the device rate), so keeping the queue near two video frames ties emulation speed to
    // the sound card's clock. Faster speeds squeeze each frame's sound by the speed factor, so
    // the queue still drains at the same rate and the game runs that much faster. Fast-forward
    // at a set rate works the same way; only unlimited fast-forward drops sound.
    void feed_audio_and_wait(bool unthrottled) {
        const std::size_t target = 2 * kSampleRate / 60;
        if (cadence_) {
            // Paced by the display: the queue absorbs the difference between the display's and
            // the sound card's clocks, steered by resampling (audio_ratio_) instead of waiting.
            const std::size_t paced_target = 3 * target / 2;
            if (audio_->queued_frames() < 4 * paced_target) audio_->push(squeezed_);
            audio_ratio_ = player::audio_ratio(sync_factor_, audio_->queued_frames(), paced_target);
            return;
        }
        if (unthrottled && audio_->queued_frames() > target) {
            return;  // drop what the device cannot play
        }
        apply_volume();
        audio_->push(squeezed_);
        while (!unthrottled && audio_->queued_frames() > target) {
            show_between(false);
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
    int pacer_speed_ = 0;
    bool fast_forward_held_ = false;
    bool fast_forward_latched_ = false;  // the quick menu's fast-forward toggle
    player::TouchStatus touch_status_;
    int file_fast_forward_ = player::kDefaultFastForward;  // what the settings file holds
    int slot_ = 1;
    player::Menu menu_;
    std::unique_ptr<platform::Context> context_;
    std::unique_ptr<platform::Window> window_;
    anygba::video::Options video_options_;
    std::unique_ptr<anygba::video::Presenter> video_;
    std::unique_ptr<anygba::video::hd::Session> hd_;
    anygba::video::Image hd_copy_;
    anygba::video::Pipeline shot_pipeline_;
    anygba::video::Pipeline preview_pipeline_, preview_plain_;
    anygba::video::Image preview_before_, preview_after_;
    std::pair<int, int> preview_size_;
    std::uint64_t preview_version_ = 0;
    bool preview_stale_ = true, preview_hd_drawn_ = false, preview_shows_hd_ = true;
    std::unique_ptr<platform::AudioStream> audio_;
    std::unique_ptr<platform::Input> input_;
    std::unique_ptr<anygba::input::DeviceSource> device_;
    NoInput no_input_;
    std::unique_ptr<platform::FramePacer> pacer_;
    std::unique_ptr<anygba::video::MotionInterpolator> motion_ = std::make_unique<anygba::video::MotionInterpolator>();
    struct Between {
        std::uint64_t at = 0;  // host time to show it
        double t = 1;          // 1: the real frame
    };
    std::vector<Between> between_;
    std::vector<std::uint8_t> between_vram_;
    std::vector<std::uint16_t> between_frame_;
    double detected_hz_ = 0;
    std::optional<player::Cadence> cadence_;
    std::uint64_t cad_frame_ = 0, cad_vblank_ = 0, cad_base_ns_ = 0;
    int vsync_set_ = 0;
    double sync_factor_ = 1.0;  // game rate over the GBA's while synced to the monitor
    double audio_ratio_ = 1.0;
    std::vector<std::uint16_t> prev_frame_;
    std::uint64_t refresh_checked_ns_ = 0;
    player::Resampler resampler_;
    player::TimeStretch stretch_;
    bool stretching_ = false;
    player::Mp2kTempo mp2k_;
    enum class DialogFor : std::uint8_t { None, SaveFolder, ExportSave, ImportSave };
    player::FileDialog dialog_;
    DialogFor dialog_for_ = DialogFor::None;
    std::unique_ptr<WavWriter> wav_;
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
    std::unique_ptr<player::TouchOverlay> touch_;
    std::unique_ptr<player::BackgroundWatch> background_;
    bool stopping_ = false;
    std::string pad_name_;
    double fps_ = 0;
    std::uint64_t fps_frames_ = 0;
    std::uint64_t fps_start_ns_ = 0;
    std::uint64_t vblank_end_ns_ = 0;
    std::uint64_t work_ns_ = 0;  // emulation time in this window, waits excluded
    std::uint64_t presents_ = 0;
    std::uint64_t window_cycles_ = 0;
    std::uint64_t window_interpreted_ = 0;
    std::vector<std::string> stats_lines_;
    player::MenuSettings menu_settings_;
    anyui::MenuView menu_view_{anygba::ui::regular_font(), anygba::ui::bold_font()};
    anyui::Canvas menu_canvas_;
    bool menu_backdrop_stale_ = true;
    std::uint64_t menu_frame_ns_ = 0;
    std::uint64_t osd_seen_ = 0;
    std::optional<std::int64_t> drag_finger_;
    float drag_y_ = 0, press_y_ = 0;
    bool dragged_ = false;
    // Declared last: destroyed first, while everything its hooks use still exists.
    std::unique_ptr<anygba::control::Controller> control_;
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

// Per-game files for this ROM in `subdir` (cheats/, symbols/) next to the program and in the
// data folder: <code>.toml, <crc32>.toml, and numbered variants such as <crc32>-2.toml.
std::vector<fs::path> game_files(const std::vector<std::uint8_t>& rom, std::uint32_t crc, std::string_view subdir) {
    std::string code(rom.begin() + 0xAC, rom.begin() + 0xB0);
    for (char& ch : code) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
    }
    char crc_name[16];
    std::snprintf(crc_name, sizeof crc_name, "%08x", crc);
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
        if (!dir || !fs::is_directory(*dir / subdir, ec)) continue;
        std::vector<fs::path> found;
        for (const auto& entry : fs::directory_iterator(*dir / subdir, ec)) {
            if (entry.is_regular_file(ec) && ours(entry.path())) found.push_back(entry.path());
        }
        std::sort(found.begin(), found.end());
        files.insert(files.end(), found.begin(), found.end());
    }
    return files;
}

// Cheat databases for this ROM (game_files in cheats/), then --cheat-db files.
void load_cheat_databases(const Args& args, const std::vector<std::uint8_t>& rom, cheats::CheatList& list) {
    const std::uint32_t crc = anygba::crc32(rom);
    std::vector<fs::path> files = game_files(rom, crc, "cheats");
    files.insert(files.end(), args.cheat_dbs.begin(), args.cheat_dbs.end());
    for (const fs::path& f : files) {
        if (!list.load_database(f, crc)) {
            std::fprintf(stderr, "warning: %s is for another version of this game (CRC-32 %08x); skipped\n",
                         f.string().c_str(), crc);
        }
    }
}

// Named memory for agents: symbols/ files for this ROM, then --symbols files (later ones win).
anygba::control::Symbols load_symbols(const Args& args, const std::vector<std::uint8_t>& rom) {
    std::vector<fs::path> files = game_files(rom, anygba::crc32(rom), "symbols");
    files.insert(files.end(), args.symbol_files.begin(), args.symbol_files.end());
    anygba::control::Symbols symbols;
    for (const fs::path& f : files) {
        symbols.merge(anygba::control::Symbols::load(f));
    }
    return symbols;
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = parse(argc, argv);
    if (!args) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    if (!anygba::control::net::is_loopback_host(args->control_host)) {
        std::fprintf(stderr, "error: --control-host %s: the control server only listens on 127.0.0.1 (or localhost)\n",
                     args->control_host.c_str());
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
        anygba::control::Symbols symbols = load_symbols(*args, rom);

        for (const auto& active : anygba::overrides::activate(anygba_game_info, args->overrides)) {
            std::printf("override: %s\n", anygba::overrides::describe(active).c_str());
            if (active.on && !active.entry->verified) {
                std::fprintf(stderr,
                             "warning: override %s (%s) is not verified: run `anygba difftest` on it, then set "
                             "verified = true (docs/dev/OVERRIDES.md)\n",
                             active.entry->name, active.entry->impl);
            }
        }
        system = std::make_unique<anygba::System>(std::move(rom), &anygba_game_info, args->runtime);
        anygba::System& sys = *system;
        if (args->difftest) {
            return anygba::difftest::run(sys, anygba_game_info, args->difftest_options);
        }
        // --save-dir, else the settings file's SaveFolder (relative to that file), else the default.
        std::optional<fs::path> dir = args->save_dir;
        if (!dir && !mapping.mapping.save_folder.empty()) {
            dir = utf8_path(mapping.mapping.save_folder.c_str());
            if (dir->is_relative() && mapping.path) dir = mapping.path->parent_path() / *dir;
        }
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
        {
            anygba::control::Settings control;
            control.host = args->control_host;
            control.port = args->control_port;
            control.readonly = args->control_readonly;
            control.takeover_frames = args->takeover_frames;
            const std::optional<fs::path> data = data_dir();
            const std::optional<fs::path> control_dir = args->control_dir ? args->control_dir
                                                        : data             ? std::optional<fs::path>(*data / "control")
                                                                           : std::nullopt;
            control.token_file = control_dir ? *control_dir / (game_file_stem() + ".json") : fs::path("anygba-control.json");
            control.info["title"] = anygba_game_info.title;
            control.info["game"] = game_file_stem();
            control.info["rom_sha256"] = anygba_game_info.rom_sha256;
            std::error_code ec;
            control.info["exe"] = fs::absolute(utf8_path(argv[0]), ec).string();
            std::unique_ptr<anygba::journal::Journal> journal;
            if (const auto root = args->journal_dir ? args->journal_dir : data ? std::optional<fs::path>(*data / "journal") : std::nullopt) {
                journal = std::make_unique<anygba::journal::Journal>(*root, game_file_stem());
                journal->settings() = args->journal_settings;
            }
            player.attach_agent_features(std::move(control), std::move(symbols), std::move(journal), args->control, args->journal);
        }
        if (args->import_save) {
            if (const auto error = player.import_save(*args->import_save, false)) throw std::runtime_error(*error);
        }
        if (args->load_state) {
            player.load_state_file(*args->load_state);
        }
        std::unique_ptr<anygba::difftest::Recorder> recorder;
        if (args->record_entry) {
            if (args->control) throw std::runtime_error("--record-entry cannot be combined with --control");
            recorder = std::make_unique<anygba::difftest::Recorder>(sys, anygba_game_info, *args->record_entry,
                                                                    args->record_count, *args->record_out);
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
        if (recorder) {
            if (const std::string message = recorder->finish(); !message.empty()) std::printf("%s\n", message.c_str());
        }
        player.print_hd_stats();
        const double elapsed = static_cast<double>(anyport::platform::now_ns() - start) / 1e9;
        int status = 0;
        if (const auto error = player.finish()) {
            std::fprintf(stderr, "error: %s\n", error->c_str());
            status = 1;
        }
        if (args->export_save) {
            const auto& data = sys.backup().data();
            const auto error = data.empty() ? std::optional<std::string>("no save data to export")
                                            : anygba::save::replace_file(*args->export_save, data, false);
            if (error) {
                std::fprintf(stderr, "error: --export-save: %s\n", error->c_str());
                status = 1;
            }
        }
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
