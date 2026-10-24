#pragma once

#include <span>
#include <string>
#include <string_view>

// `anygba hdpack`: the HD pack creator (docs/user/HD-PACKS.md). Works on the folders the
// game's dump mode writes: de-duplicate palette variants, preview, batch import edited art,
// and generate a whole pack with an AI upscaler.
namespace anygba::hdpack {

std::string usage();

// args: everything after "hdpack". Returns the process exit code; messages go to stdout/stderr.
int run(std::span<const std::string_view> args);

}  // namespace anygba::hdpack
