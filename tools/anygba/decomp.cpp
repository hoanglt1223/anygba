#include "decomp.h"

#include "build.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace anygba::driver {

namespace fs = std::filesystem;

namespace {

std::optional<std::string> env(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return std::nullopt;
    std::string out(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (!value) return std::nullopt;
    std::string out(value);
#endif
    if (out.empty()) return std::nullopt;
    return out;
}

std::optional<fs::path> find_on_path(const std::string& name) {
    const auto path = env("PATH");
    if (!path) return std::nullopt;
#if defined(_WIN32)
    const char sep = ';';
    const std::vector<std::string> suffixes{".exe", ""};
#else
    const char sep = ':';
    const std::vector<std::string> suffixes{""};
#endif
    std::size_t pos = 0;
    while (pos <= path->size()) {
        const std::size_t end = path->find(sep, pos);
        const std::string dir = path->substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!dir.empty()) {
            for (const auto& s : suffixes) {
                std::error_code ec;
                const fs::path candidate = fs::path(dir) / (name + s);
                // The Windows Store alias opens the Store instead of running Python.
                if (fs::is_regular_file(candidate, ec) && candidate.string().find("WindowsApps") == std::string::npos) {
                    return candidate;
                }
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return std::nullopt;
}

std::vector<std::string> python_command() {
    if (const auto forced = env("ANYGBA_PYTHON")) return {*forced};
    for (const char* name : {"python3", "python"}) {
        if (const auto p = find_on_path(name)) return {p->string()};
    }
#if defined(_WIN32)
    if (const auto p = find_on_path("py")) return {p->string(), "-3"};
#endif
    throw std::runtime_error("Python 3.11 or newer is needed; put it on the PATH or set ANYGBA_PYTHON");
}

std::optional<fs::path> installed_prefix() {
    try {
        const fs::path prefix = default_prefix();
        std::error_code ec;
        if (fs::is_directory(prefix / "lib" / "cmake" / "anygba", ec)) return prefix;
    } catch (const std::exception&) {
        // not an installed anygba: the user passes --prefix
    }
    return std::nullopt;
}

fs::path find_script(const std::optional<fs::path>& prefix) {
    std::vector<fs::path> candidates;
    if (prefix) candidates.push_back(*prefix / "share" / "anygba" / "decomp" / "decomp.py");
#ifdef ANYGBA_SOURCE_DIR
    candidates.push_back(fs::path(ANYGBA_SOURCE_DIR) / "tools" / "decomp" / "decomp.py");
#endif
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec)) return c;
    }
    throw std::runtime_error("cannot find decomp.py (share/anygba/decomp in the install prefix)");
}

}  // namespace

std::string decomp_usage() {
    return "usage: anygba decomp <rom.gba> --func NAME|0xADDR[:thumb|arm] [--func ...] [options]\n"
           "\n"
           "Rewrites guest functions as C++ overrides: Ghidra exports each function, an LLM writes the override,\n"
           "and the differential test decides; failures go back to the model (docs/dev/OVERRIDES.md).\n"
           "Output goes to the per-user data folder (decomp/<game>), never into the repository.\n"
           "\n"
           "  --funcs FILE            functions, one per line (batch; callees first)\n"
           "  --symbols FILE          [functions] table naming functions; repeatable\n"
           "  --engine E              claude-cli (claude -p, default when installed), anthropic-api\n"
           "                          (ANTHROPIC_API_KEY), or none (export and scaffold only)\n"
           "  --model NAME            model for the engine\n"
           "  --attempts N            model answers per function (default 4)\n"
           "  --states N, --seed N    random difftest states (default 10000, seed 1)\n"
           "  --load-state FILE       save state the random states start from\n"
           "  --recorded FILE         states from <game> --record-entry; repeatable\n"
           "  --spec FILE             argument specs to use as they are\n"
           "  --miss-log FILE, --config FILE   passed to the recompiler\n"
           "  --ghidra DIR, --java-home DIR    Ghidra 12.2 and Java 25 (or ANYGBA_GHIDRA, ANYGBA_JAVA_HOME)\n"
           "  --work DIR, --prefix DIR, --generator G\n"
           "\n"
           "Needs Python 3.11+ (ANYGBA_PYTHON), CMake and a C++ compiler, Ghidra, and for the LLM the Claude Code\n"
           "CLI or an API key. Exit code 0: every function passed; 1: some did not; 2: error.\n";
}

int decomp(std::span<const std::string_view> args) {
    for (const auto a : args) {
        if (a == "--help" || a == "-h") {
            std::fputs(decomp_usage().c_str(), stdout);
            return 0;
        }
    }
    try {
        const auto prefix = installed_prefix();
        std::vector<std::string> argv = python_command();
        argv.push_back(find_script(prefix).string());
        bool has_prefix = false;
        for (const auto a : args) {
            has_prefix = has_prefix || a == "--prefix";
            argv.emplace_back(a);
        }
        if (!has_prefix && prefix) argv.insert(argv.end(), {"--prefix", prefix->string()});
        const int rc = run_process(argv);
        return rc == 0 || rc == 1 ? rc : 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "anygba decomp: %s\n", e.what());
        return 2;
    }
}

}  // namespace anygba::driver
