#include "build.h"

#include "anygba/crc32.h"

#include "anygba/embedded_rom.h"
#include "cli.h"
#include "pipeline.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace anygba::driver {

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quoting that CommandLineToArgvW and the MSVC runtime undo exactly.
std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out += ch;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}
#endif

std::string path_string(const fs::path& p) {
    const std::u8string u = p.u8string();
    return {u.begin(), u.end()};
}

std::string exe_suffix() {
#if defined(_WIN32)
    return ".exe";
#else
    return "";
#endif
}

void run_checked(const std::vector<std::string>& argv, const char* what) {
    std::string line;
    for (const auto& a : argv) line += (line.empty() ? "" : " ") + a;
    std::printf("anygba: %s\n", line.c_str());
    std::fflush(stdout);
    const int rc = run_process(argv);
    if (rc != 0) {
        throw std::runtime_error(std::string(what) + " failed (exit code " + std::to_string(rc) + ")");
    }
}

fs::path default_prefix() {
    const auto exe = executable_path();
    if (!exe) {
        throw std::runtime_error("cannot locate this program; pass --prefix <anygba install prefix>");
    }
    return exe->parent_path().parent_path();  // <prefix>/bin/anygba
}

// Cheat databases for this ROM shipped in <prefix>/share/anygba/cheats: <game code>.toml and
// <crc32>.toml; build() copies them where the game looks for them (docs/user/CHEATS.md).
std::vector<fs::path> cheat_databases(const fs::path& prefix, const std::vector<std::uint8_t>& rom) {
    std::vector<fs::path> found;
    if (rom.size() < 0xB0) return found;
    std::string code(rom.begin() + 0xAC, rom.begin() + 0xB0);
    for (char& ch : code) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
    }
    char crc[16];
    std::snprintf(crc, sizeof crc, "%08x", anygba::crc32(rom));
    for (const std::string& name : {code + ".toml", std::string(crc) + ".toml"}) {
        const fs::path p = prefix / "share" / "anygba" / "cheats" / name;
        if (fs::exists(p)) found.push_back(p);
    }
    return found;
}

std::vector<std::uint8_t> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + p.string());
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

int run_process(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        throw std::invalid_argument("run_process: empty command");
    }
#if defined(_WIN32)
    std::wstring line;
    for (const auto& a : argv) {
        line += (line.empty() ? L"" : L" ") + quote(widen(a));
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        throw std::runtime_error("cannot start " + argv[0] + " (error " + std::to_string(GetLastError()) + ")");
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0) {
        throw std::runtime_error("cannot start " + argv[0]);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        throw std::runtime_error("waiting for " + argv[0] + " failed");
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
}

std::string program_name(std::string_view title) {
    std::string out;
    for (const char ch : title) {
        const bool keep = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                          ch == '_' || ch == ' ';
        if (keep) out += ch;
    }
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) {
        return "game";
    }
    return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

std::string build_usage() {
    return "usage: anygba build <game.gba> [options]\n"
           "\n"
           "Recompiles the ROM, compiles the result against the installed runtime, and writes\n"
           "<Title>.exe (Windows), <Title> (Linux) or <Title>.app (macOS) with the ROM embedded.\n"
           "\n"
           "  --config FILE      per-game config (docs/dev/GAME-CONFIG.md)\n"
           "  --miss-log FILE    trace-feedback log from an earlier run (repeatable)\n"
           "  -o, --output DIR   where the program goes (default: current folder)\n"
           "  --work DIR         generated C and build files (default: <output>/<rom name>-build)\n"
           "  --name NAME        program name (default: the game title)\n"
           "  --prefix DIR       anygba install prefix (default: the folder above this program's bin/)\n"
           "  --cmake PATH       cmake executable (default: cmake on PATH)\n"
           "  --generator NAME   CMake generator (default: CMake's choice)\n"
           "  --build-type TYPE  CMake configuration (default: Release)\n"
           "  --no-embed         do not embed the ROM; the program then takes it as its first argument\n";
}

std::optional<std::string> parse_build_args(std::span<const std::string_view> args, BuildOptions& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        const bool has_value = i + 1 < args.size();
        const auto value = [&]() { return std::string(args[++i]); };
        if (a == "--no-embed") {
            out.embed_rom = false;
        } else if (a.starts_with("-") && !has_value) {
            return std::string(a) + " needs a value or is unknown";
        } else if (a == "--config") {
            out.config = fs::path(value());
        } else if (a == "--miss-log") {
            out.miss_logs.emplace_back(value());
        } else if (a == "-o" || a == "--output") {
            out.output_dir = value();
        } else if (a == "--work") {
            out.work_dir = fs::path(value());
        } else if (a == "--name") {
            out.name = program_name(value());
        } else if (a == "--prefix") {
            out.prefix = fs::path(value());
        } else if (a == "--cmake") {
            out.cmake = value();
        } else if (a == "--generator") {
            out.generator = value();
        } else if (a == "--build-type") {
            out.build_type = value();
        } else if (a.starts_with("-")) {
            return "unknown option " + std::string(a);
        } else if (out.rom.empty()) {
            out.rom = fs::path(std::string(a));
        } else {
            return "unexpected argument " + std::string(a);
        }
    }
    if (out.rom.empty()) {
        return std::string("no ROM given");
    }
    return std::nullopt;
}

fs::path build(const BuildOptions& o) {
    recomp::Options ro;
    ro.rom = o.rom;
    ro.config = o.config;
    ro.miss_logs = o.miss_logs;
    const fs::path work = o.work_dir ? *o.work_dir : o.output_dir / (o.rom.stem().string() + "-build");
    ro.output_dir = work / "gen";
    if (const auto error = recomp::validate_inputs(ro)) {
        throw std::runtime_error(*error);
    }
    const recomp::Analysis analysis = recomp::analyze(ro);
    const std::size_t count = recomp::generate(ro, analysis);
    std::printf("anygba: %zu functions written to %s\n", count, path_string(ro.output_dir).c_str());

    const fs::path prefix = fs::absolute(o.prefix ? *o.prefix : default_prefix());
    if (!fs::exists(prefix / "lib" / "cmake" / "anygba")) {
        throw std::runtime_error("no anygba runtime package under " + path_string(prefix) +
                                 " (expected lib/cmake/anygba); pass --prefix");
    }
    const fs::path bin = work / "out";
    std::vector<std::string> configure = {o.cmake, "-S", path_string(ro.output_dir), "-B", path_string(bin),
                                          "-DCMAKE_PREFIX_PATH=" + path_string(prefix),
                                          "-DCMAKE_BUILD_TYPE=" + o.build_type};
    if (o.generator) {
        configure.push_back("-G");
        configure.push_back(*o.generator);
    }
    run_checked(configure, "CMake configure");
    run_checked({o.cmake, "--build", path_string(bin), "--config", o.build_type}, "CMake build");

    fs::path built;
    for (const auto& candidate : {bin / o.build_type / ("game" + exe_suffix()), bin / ("game" + exe_suffix())}) {
        if (fs::exists(candidate)) {
            built = candidate;
            break;
        }
    }
    if (built.empty()) {
        throw std::runtime_error("the build produced no game executable under " + path_string(bin));
    }

    const std::string name = o.name ? *o.name : program_name(analysis.title);
    fs::create_directories(o.output_dir);
    const std::vector<fs::path> databases = cheat_databases(prefix, read_file(o.rom));
    const auto copy_databases = [&databases](const fs::path& dir) {
        if (databases.empty()) return;
        fs::create_directories(dir);
        for (const auto& db : databases) {
            fs::copy_file(db, dir / db.filename(), fs::copy_options::overwrite_existing);
            std::printf("anygba: cheat database %s\n", path_string(dir / db.filename()).c_str());
        }
    };
#if defined(__APPLE__)
    const fs::path app = o.output_dir / (name + ".app");
    fs::remove_all(app);
    fs::create_directories(app / "Contents" / "MacOS");
    fs::create_directories(app / "Contents" / "Resources");
    fs::copy_file(built, app / "Contents" / "MacOS" / name);
    if (o.embed_rom) {
        fs::copy_file(o.rom, app / "Contents" / "Resources" / "game.gba");
    }
    std::ofstream plist(app / "Contents" / "Info.plist", std::ios::binary);
    plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\"><dict>\n"
             "  <key>CFBundleExecutable</key><string>" << name << "</string>\n"
             "  <key>CFBundleIdentifier</key><string>io.anyport.anygba.game</string>\n"
             "  <key>CFBundleName</key><string>" << name << "</string>\n"
             "  <key>CFBundlePackageType</key><string>APPL</string>\n"
             "</dict></plist>\n";
    plist.close();
    if (!plist) {
        throw std::runtime_error("cannot write " + path_string(app / "Contents" / "Info.plist"));
    }
    copy_databases(app / "Contents" / "Resources" / "cheats");  // SDL_GetBasePath() of a bundle
    run_checked({"codesign", "--force", "--deep", "--sign", "-", path_string(app)}, "ad-hoc signing");
    return app;
#else
    const fs::path exe = o.output_dir / (name + exe_suffix());
    fs::copy_file(built, exe, fs::copy_options::overwrite_existing);
    if (o.embed_rom) {
        append_rom(exe, read_file(o.rom));
    }
    copy_databases(o.output_dir / "cheats");
    return exe;
#endif
}

}  // namespace anygba::driver
