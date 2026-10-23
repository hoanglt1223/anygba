#include "cli.h"

#include <cstdlib>
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
        if (arg == "--list") {
            result.options.list_only = true;
            continue;
        }
        if (arg == "--shards") {
            if (i + 1 >= args.size()) {
                return fail("--shards needs a value");
            }
            const std::string value(args[++i]);
            const auto n = std::strtoul(value.c_str(), nullptr, 10);
            if (n == 0 || n > 4096 || value.find_first_not_of("0123456789") != std::string::npos) {
                return fail("--shards must be a number from 1 to 4096");
            }
            result.options.shards = n;
            continue;
        }
        if (arg == "--config" || arg == "-o" || arg == "--output" || arg == "--miss-log") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                return fail(std::string(arg) + " needs a value");
            }
            const std::filesystem::path value(args[++i]);
            if (arg == "--config") {
                result.options.config = value;
            } else if (arg == "--miss-log") {
                result.options.miss_logs.push_back(value);
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
    return "usage: anygba-recomp <rom.gba> [--config game.toml] [--miss-log FILE]... [--output dir] [--list]\n"
           "\n"
           "  --config FILE   per-game TOML configuration\n"
           "  --miss-log FILE runtime miss log; its addresses become entry points (repeatable)\n"
           "  -o, --output    directory for the generated C project (default: recomp-out)\n"
           "  --list          print discovered functions with disassembly instead of generating C\n"
           "  --shards N      split the functions over exactly N funcs_NNN.c files\n"
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
    for (const auto& log : options.miss_logs) {
        if (!std::filesystem::is_regular_file(log, ec)) {
            return "miss log not found: " + log.string();
        }
    }
    return std::nullopt;
}

}  // namespace anygba::recomp
