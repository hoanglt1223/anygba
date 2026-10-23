#include "cli.h"

#include <cstdio>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    using namespace anygba::recomp;

    const std::vector<std::string_view> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    const ParseResult parsed = parse_args(args);

    switch (parsed.action) {
    case Action::Help:
        std::fputs(usage().c_str(), stdout);
        return 0;
    case Action::Version:
        std::puts(version().c_str());
        return 0;
    case Action::Error:
        std::fprintf(stderr, "anygba-recomp: %s\n\n%s", parsed.error.c_str(), usage().c_str());
        return 2;
    case Action::Run:
        break;
    }

    if (const auto error = validate_inputs(parsed.options)) {
        std::fprintf(stderr, "anygba-recomp: %s\n", error->c_str());
        return 1;
    }
    std::fputs("anygba-recomp: the recompiler pipeline is not implemented yet\n", stderr);
    return 1;
}
