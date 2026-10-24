#include "build.h"
#include "hdpack.h"

#include <cstdio>
#include <exception>
#include <span>
#include <string_view>
#include <vector>

namespace {

const char* const kUsage =
    "usage: anygba <command> [arguments]\n"
    "\n"
    "Commands:\n"
    "  build <game.gba> [options]   recompile and compile a ROM into a native program\n"
    "  hdpack <command> ...         HD pack creator: dedupe, preview, import, ai-upscale\n"
    "  help [command]               show help\n"
    "\n"
    "Run `anygba help build` or `anygba help hdpack` for the options. The recompiler alone is\n"
    "anygba-recomp.\n";

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        if (args.size() > 1 && args[1] == "build") {
            std::fputs(anygba::driver::build_usage().c_str(), stdout);
        } else if (args.size() > 1 && args[1] == "hdpack") {
            std::fputs(anygba::hdpack::usage().c_str(), stdout);
        } else {
            std::fputs(kUsage, args.empty() ? stderr : stdout);
        }
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "hdpack") {
        return anygba::hdpack::run(std::span<const std::string_view>(args.data() + 1, args.size() - 1));
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
