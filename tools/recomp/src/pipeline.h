#pragma once

#include "analysis/functions.h"
#include "analysis/image.h"
#include "cli.h"

#include <iosfwd>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace anygba::recomp {

struct Analysis {
    std::string title;
    std::string rom_sha256;
    std::uint32_t rom_size = 0;
    std::string save_type;
    int rtc = -1;
    std::map<std::uint32_t, std::string> replacements;
    std::vector<std::filesystem::path> sources;
    std::unique_ptr<analysis::Image> image;
    analysis::Discovery discovery;
};

// Loads the ROM, config, and miss logs named in `options` and runs function discovery.
// Throws std::runtime_error (or a config/miss-log error) with a user-facing message.
Analysis analyze(const Options& options);

void write_listing(std::ostream& out, const Analysis& analysis);

// Writes the C project for `analysis` to options.output_dir; returns the number of functions.
std::size_t generate(const Options& options, const Analysis& analysis);

}  // namespace anygba::recomp
