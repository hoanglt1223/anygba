#include "difftest.h"

#include "build.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <stdexcept>
#include <vector>

namespace anygba::driver {

namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool is_program(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
#if defined(_WIN32)
    return p.extension() == ".exe";
#else
    const auto perms = fs::status(p, ec).permissions();
    return (perms & fs::perms::owner_exec) != fs::perms::none && p.extension().empty();
#endif
}

// A game program, or the folder `anygba build` wrote it to.
fs::path find_game(const fs::path& given) {
    std::error_code ec;
    if (!fs::is_directory(given, ec)) {
        if (!fs::exists(given, ec)) throw std::runtime_error("no such game program or folder: " + given.string());
        return given;
    }
    std::vector<fs::path> found;
    for (const auto& item : fs::directory_iterator(given, ec)) {
        if (is_program(item.path())) found.push_back(item.path());
    }
    if (found.size() == 1) return found[0];
    std::string list;
    for (const auto& f : found) list += "\n  " + f.string();
    throw std::runtime_error(given.string() + (found.empty() ? " holds no game program" :
                                               " holds several programs; name one:" + list));
}

std::optional<std::string> json_field(const std::string& json, const std::string& pattern) {
    std::smatch m;
    if (std::regex_search(json, m, std::regex(pattern))) return m[1].str();
    return std::nullopt;
}

std::string today() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

}  // namespace

std::string difftest_usage() {
    return "usage: anygba difftest <game program | folder> --func NAME [options]\n"
           "\n"
           "Runs the override of NAME (its symbol or entry address) and the recompiled original from the same\n"
           "start states and compares them (docs/dev/OVERRIDES.md). Exit code 0: pass, 1: fail, 2: error,\n"
           "3: insufficient (no ABI difference, but fewer states than the spec's min_returned returned normally).\n"
           "\n"
           "  --rom FILE          the ROM, when the program does not embed it\n"
           "  --overrides DIR     the override module: its difftest.toml is the argument spec\n"
           "  --spec FILE         argument spec (default: <module>/difftest.toml)\n"
           "  --states N          random states (default 10000)\n"
           "  --seed N            random seed (default 1)\n"
           "  --load-state FILE   save state the random states start from (default: the reset state)\n"
           "  --recorded FILE     states written by `<game> --record-entry NAME --count N --record-out FILE`;\n"
           "                      repeatable\n"
           "  --impl NAME         test another implementation compiled into the program\n"
           "  --tick-limit N      a run longer than N scanlines of cycles is stuck\n"
           "  --json FILE         write the report as JSON\n"
           "  --mark-verified     after a pass, set verified = true for this override in <module>/overrides.toml\n";
}

int difftest(std::span<const std::string_view> args) {
    std::optional<fs::path> game, rom, module, spec, json;
    std::optional<std::string> func, impl;
    bool mark = false;
    std::vector<std::string> pass;  // options handed to the game unchanged
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--help" || a == "-h") {
            std::fputs(difftest_usage().c_str(), stdout);
            return 0;
        }
        if (a == "--mark-verified") {
            mark = true;
            continue;
        }
        if (!a.starts_with("-")) {
            if (game) {
                std::fprintf(stderr, "anygba difftest: more than one game program given\n\n%s", difftest_usage().c_str());
                return 2;
            }
            game = fs::path(std::string(a));
            continue;
        }
        if (i + 1 >= args.size()) {
            std::fprintf(stderr, "anygba difftest: %.*s needs a value\n\n%s", static_cast<int>(a.size()), a.data(),
                         difftest_usage().c_str());
            return 2;
        }
        const std::string value(args[++i]);
        if (a == "--func") {
            func = value;
        } else if (a == "--rom") {
            rom = value;
        } else if (a == "--overrides") {
            module = value;
        } else if (a == "--spec") {
            spec = value;
        } else if (a == "--json") {
            json = value;
        } else if (a == "--impl") {
            impl = value;
        } else if (a == "--states" || a == "--seed" || a == "--load-state" || a == "--recorded" || a == "--tick-limit") {
            pass.emplace_back(a);
            pass.push_back(value);
        } else {
            std::fprintf(stderr, "anygba difftest: unknown option %.*s\n\n%s", static_cast<int>(a.size()), a.data(),
                         difftest_usage().c_str());
            return 2;
        }
    }
    if (!game || !func) {
        std::fprintf(stderr, "anygba difftest: give the game program and --func\n\n%s", difftest_usage().c_str());
        return 2;
    }
    try {
        if (mark && !module) throw std::runtime_error("--mark-verified needs --overrides DIR (the module to update)");
        if (mark && impl) throw std::runtime_error("--mark-verified records the configured implementation; drop --impl");
        if (!spec && module && fs::exists(*module / "difftest.toml")) spec = *module / "difftest.toml";
        // Absolute with native separators: CreateProcess does not resolve "dir/game.exe".
        std::vector<std::string> argv{fs::absolute(find_game(*game)).make_preferred().string()};
        if (rom) argv.push_back(rom->string());
        argv.insert(argv.end(), {"--difftest", *func});
        if (spec) argv.insert(argv.end(), {"--spec", spec->string()});
        if (impl) argv.insert(argv.end(), {"--impl", *impl});
        fs::path report = json ? *json : fs::temp_directory_path() / "anygba-difftest-report.json";
        if (json || mark) argv.insert(argv.end(), {"--json", report.string()});
        argv.insert(argv.end(), pass.begin(), pass.end());
        const int rc = run_process(argv);
        if (rc != 0 || !mark) return rc == 0 || rc == 1 || rc == 3 ? rc : 2;

        const std::string text = read_text(report);
        if (!json) {
            std::error_code ec;
            fs::remove(report, ec);
        }
        const auto configured = json_field(text, R"re("configured_impl":"([^"]*)")re");
        const auto total = json_field(text, R"re("total":([0-9]+))re");
        const auto percent = json_field(text, R"re("percent":([0-9.eE+-]+))re");
        const auto returned = json_field(text, R"re("returned_share":([0-9.eE+-]+))re");
        if (!configured || !total || !returned) {
            throw std::runtime_error("the difftest report lacks the fields --mark-verified needs");
        }
        char cov[32] = "unknown";
        if (percent) std::snprintf(cov, sizeof cov, "%.1f%%", std::strtod(percent->c_str(), nullptr));
        char ret[32];
        std::snprintf(ret, sizeof ret, "%.1f%%", 100.0 * std::strtod(returned->c_str(), nullptr));
        const std::string comment = "anygba difftest passed " + today() + ": " + *total + " states (" + ret +
                                    " returned normally), block coverage " + cov;
        mark_verified(*module / "overrides.toml", *configured, comment);
        std::printf("anygba: %s: verified = true for %s (%s)\n", (*module / "overrides.toml").string().c_str(),
                    configured->c_str(), comment.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "anygba difftest: %s\n", e.what());
        return 2;
    }
}

void mark_verified(const fs::path& toml, const std::string& impl, const std::string& comment) {
    const std::string text = read_text(toml);
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    for (std::size_t pos = 0; pos <= text.size();) {
        const std::size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (end == std::string::npos) {
            if (!line.empty()) lines.push_back(line);
            break;
        }
        lines.push_back(line);
        pos = end + 1;
    }
    const std::regex header(R"(^\s*\[\[override\]\]\s*(#.*)?$)");
    const std::regex any_header(R"(^\s*\[)");
    const std::regex impl_line("^\\s*impl\\s*=\\s*\"" + impl + "\"\\s*(#.*)?$");
    const std::regex verified_line(R"(^\s*verified\s*=)");
    const std::string entry = "verified = true  # " + comment;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!std::regex_search(lines[i], header)) continue;
        std::size_t end = i + 1;
        while (end < lines.size() && !std::regex_search(lines[end], any_header)) ++end;
        std::optional<std::size_t> impl_at, verified_at;
        for (std::size_t k = i + 1; k < end; ++k) {
            if (std::regex_search(lines[k], impl_line)) impl_at = k;
            if (std::regex_search(lines[k], verified_line)) verified_at = k;
        }
        if (!impl_at) continue;
        if (verified_at) {
            lines[*verified_at] = entry;
        } else {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(*impl_at + 1), entry);
        }
        std::string out;
        for (const auto& l : lines) out += l + eol;
        std::ofstream f(toml, std::ios::binary | std::ios::trunc);
        f << out;
        f.close();
        if (!f) throw std::runtime_error("cannot write " + toml.string());
        return;
    }
    throw std::runtime_error(toml.string() + " has no [[override]] with impl = \"" + impl + "\"");
}

}  // namespace anygba::driver
