#include "cli.h"
#include "pipeline.h"

#include <cstdio>
#include <exception>
#include <iostream>
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
    try {
        const Analysis analysis = analyze(parsed.options);
        if (parsed.options.list_only) {
            write_listing(std::cout, analysis);
            return 0;
        }
        const std::size_t count = generate(parsed.options, analysis);
        std::printf("anygba-recomp: %zu functions written to %s\n", count, parsed.options.output_dir.string().c_str());
        for (const auto& note : analysis.discovery.notes) {
            std::fprintf(stderr, "note: %s\n", note.c_str());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "anygba-recomp: %s\n", e.what());
        return 1;
    }
}
