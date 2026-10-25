#pragma once

#include <span>
#include <string>
#include <string_view>

namespace anygba::driver {

// `anygba decomp <rom> --func NAME|ADDR ...`: runs tools/decomp/decomp.py (installed to
// share/anygba/decomp) with Python 3.11+, passing this install's prefix (docs/dev/OVERRIDES.md).
std::string decomp_usage();
int decomp(std::span<const std::string_view> args);

}  // namespace anygba::driver
