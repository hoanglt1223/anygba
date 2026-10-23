#pragma once

#include "analysis/functions.h"
#include "analysis/image.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace anygba::io {

class MissLogError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One address the runtime had to interpret because no recompiled function started there.
// Line format, shared with the runtime's writer:  0x03000124 arm [rom=0x08001234]
// `rom` names the ROM bytes the runtime found at that RAM address, if any.
struct MissRecord {
    std::uint32_t addr = 0;
    analysis::Mode mode = analysis::Mode::Arm;
    std::optional<std::uint32_t> rom_src;
};

std::vector<MissRecord> parse_miss_log(std::string_view text, std::string_view source_name);
std::vector<MissRecord> read_miss_log(const std::filesystem::path& path);
std::string format_miss_record(const MissRecord& record);

// Adds each record as an entry point. RAM addresses the image cannot see yet get a region
// backed by the reported ROM bytes, extending to the end of that RAM or of the ROM.
void apply_miss_log(const std::vector<MissRecord>& records, analysis::Image& image,
                    std::vector<analysis::EntryPoint>& entries);

}  // namespace anygba::io
