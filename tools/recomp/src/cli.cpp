#include "cli.h"

#include <system_error>

namespace anygba::recomp {

namespace {

ParseResult fail(std::string message) {
    ParseResult result;
    result.action = Action::Error;
    result.error = std::move(message);
    return result;
}

}  // namespace

ParseResult parse_args(std::span<const std::string_view> args) {
    ParseResult result;
    bool have_rom = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            result.action = Action::Help;
            return result;
        }
        if (arg == "--version") {
            result.action = Action::Version;
            return result;
        }
        if (arg == "--config" || arg == "-o" || arg == "--output") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                return fail(std::string(arg) + " needs a value");
            }
            const std::filesystem::path value(args[++i]);
            if (arg == "--config") {
                result.options.config = value;
            } else {
                result.options.output_dir = value;
            }
            continue;
        }
        if (arg.starts_with("-")) {
            return fail("unknown option: " + std::string(arg));
        }
        if (have_rom) {
            return fail("only one ROM may be given, got a second: " + std::string(arg));
        }
        result.options.rom = std::filesystem::path(arg);
        have_rom = true;
    }

    if (!have_rom) {
        return fail("missing ROM path");
    }
    result.action = Action::Run;
    return result;
}

std::string usage() {
    return "usage: anygba-recomp <rom.gba> [--config game.toml] [--output dir]\n"
           "\n"
           "  --config FILE   per-game TOML configuration\n"
           "  -o, --output    directory for the generated C project (default: recomp-out)\n"
           "  -h, --help      show this help\n"
           "  --version       show the version\n";
}

std::string version() {
    return "anygba-recomp " ANYGBA_VERSION;
}

std::optional<std::string> validate_inputs(const Options& options) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(options.rom, ec)) {
        return "ROM not found: " + options.rom.string();
    }
    if (options.config && !std::filesystem::is_regular_file(*options.config, ec)) {
        return "config not found: " + options.config->string();
    }
    return std::nullopt;
}

}  // namespace anygba::recomp
