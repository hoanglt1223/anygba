#include "build.h"
#include "decomp.h"
#include "difftest.h"
#include "hdpack.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

const char* const kUsage =
    "usage: anygba <command> [arguments]\n"
    "\n"
    "Commands:\n"
    "  build <game.gba> [options]   recompile and compile a ROM into a native program or an Android app\n"
    "  android-launcher [options]   build the AnyGBA launcher app (AnyGBA-Launcher.apk) for game packages\n"
    "  hdpack <command> ...         HD pack creator: dedupe, preview, import, ai-upscale\n"
    "  difftest <game> --func NAME  compare a function override with the recompiled original\n"
    "  decomp <game.gba> --func F   rewrite guest functions as overrides with Ghidra and an LLM\n"
    "  help [command]               show help\n"
    "\n"
    "Run `anygba help <command>` (build, hdpack, difftest, decomp) for the options. The recompiler alone is\n"
    "anygba-recomp.\n";

const char* const kLauncherUsage =
    "usage: anygba android-launcher [-o DIR] [--prefix DIR] [--work DIR] [Android options]\n"
    "\n"
    "Writes AnyGBA-Launcher.apk, the app that runs game packages made with\n"
    "`anygba build game.gba --target android-package`.\n"
    "\n";

int android_launcher(std::span<const std::string_view> args) {
    namespace fs = std::filesystem;
    anygba::driver::android::Options options;
    anygba::driver::android::Paths paths;
    paths.output_dir = ".";
    std::optional<fs::path> prefix, work;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--help" || a == "-h") {
            std::fputs(kLauncherUsage, stdout);
            std::fputs(anygba::driver::android::usage().c_str(), stdout);
            return 0;
        }
        if (const auto error = anygba::driver::android::parse_option(args, i, options)) {
            if (error->empty()) continue;
            std::fprintf(stderr, "anygba android-launcher: %s\n", error->c_str());
            return 2;
        }
        if (i + 1 >= args.size()) {
            std::fprintf(stderr, "anygba android-launcher: unknown option or missing value: %.*s\n\n%s",
                         static_cast<int>(a.size()), a.data(), kLauncherUsage);
            return 2;
        }
        const fs::path value{std::string(args[++i])};
        if (a == "-o" || a == "--output") {
            paths.output_dir = value;
        } else if (a == "--prefix") {
            prefix = value;
        } else if (a == "--work") {
            work = value;
        } else {
            std::fprintf(stderr, "anygba android-launcher: unknown option %.*s\n\n%s", static_cast<int>(a.size()),
                         a.data(), kLauncherUsage);
            return 2;
        }
    }
    try {
        paths.prefix = fs::absolute(prefix ? *prefix : anygba::driver::default_prefix());
        paths.work = work ? *work : paths.output_dir / "launcher-build";
        const auto out = anygba::driver::android::build_launcher(paths, options);
        std::printf("anygba: wrote %s\n", out.string().c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "anygba android-launcher: %s\n", e.what());
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        if (args.size() > 1 && args[1] == "build") {
            std::fputs(anygba::driver::build_usage().c_str(), stdout);
        } else if (args.size() > 1 && args[1] == "hdpack") {
            std::fputs(anygba::hdpack::usage().c_str(), stdout);
        } else if (args.size() > 1 && args[1] == "difftest") {
            std::fputs(anygba::driver::difftest_usage().c_str(), stdout);
        } else if (args.size() > 1 && args[1] == "decomp") {
            std::fputs(anygba::driver::decomp_usage().c_str(), stdout);
        } else {
            std::fputs(kUsage, args.empty() ? stderr : stdout);
        }
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "android-launcher") {
        return android_launcher(std::span<const std::string_view>(args.data() + 1, args.size() - 1));
    }
    if (args[0] == "hdpack") {
        return anygba::hdpack::run(std::span<const std::string_view>(args.data() + 1, args.size() - 1));
    }
    if (args[0] == "difftest") {
        return anygba::driver::difftest(std::span<const std::string_view>(args.data() + 1, args.size() - 1));
    }
    if (args[0] == "decomp") {
        return anygba::driver::decomp(std::span<const std::string_view>(args.data() + 1, args.size() - 1));
    }
    if (args[0] != "build") {
        std::fprintf(stderr, "anygba: unknown command '%.*s'\n\n%s", static_cast<int>(args[0].size()), args[0].data(),
                     kUsage);
        return 2;
    }
    anygba::driver::BuildOptions options;
    const std::span<const std::string_view> rest(args.data() + 1, args.size() - 1);
    if (rest.size() == 1 && (rest[0] == "--help" || rest[0] == "-h")) {
        std::fputs(anygba::driver::build_usage().c_str(), stdout);
        return 0;
    }
    if (const auto error = anygba::driver::parse_build_args(rest, options)) {
        std::fprintf(stderr, "anygba build: %s\n\n%s", error->c_str(), anygba::driver::build_usage().c_str());
        return 2;
    }
    try {
        const auto out = anygba::driver::build(options);
        const auto u = out.u8string();
        std::printf("anygba: wrote %s\n", std::string(u.begin(), u.end()).c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "anygba build: %s\n", e.what());
        return 1;
    }
}
