#pragma once

#include "analysis/functions.h"
#include "analysis/image.h"
#include "cli.h"

#include <iosfwd>
#include <memory>

namespace anygba::recomp {

struct Analysis {
    std::unique_ptr<analysis::Image> image;
    analysis::Discovery discovery;
};

// Loads the ROM, config, and miss logs named in `options` and runs function discovery.
// Throws std::runtime_error (or a config/miss-log error) with a user-facing message.
Analysis analyze(const Options& options);

void write_listing(std::ostream& out, const Analysis& analysis);

}  // namespace anygba::recomp
