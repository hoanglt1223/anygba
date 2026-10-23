#include "io/miss_log.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace anygba::io {

namespace {

std::optional<std::uint32_t> parse_hex(std::string_view s) {
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s.remove_prefix(2);
    }
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 16);
    if (s.empty() || ec != std::errc() || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (i > start) {
            out.push_back(line.substr(start, i - start));
        }
    }
    return out;
}

}  // namespace

std::vector<MissRecord> parse_miss_log(std::string_view text, std::string_view source_name) {
    std::vector<MissRecord> records;
    std::set<std::uint32_t> seen;
    std::size_t line_no = 0;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        ++line_no;
        if (const auto hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const auto fields = split(line);
        if (fields.empty()) {
            continue;
        }
        const auto fail = [&](const std::string& why) {
            throw MissLogError(std::string(source_name) + ":" + std::to_string(line_no) + ": " + why);
        };
        if (fields.size() < 2 || fields.size() > 3) {
            fail("expected '<addr> <arm|thumb> [rom=<addr>]'");
        }
        MissRecord record;
        const auto addr = parse_hex(fields[0]);
        if (!addr) {
            fail("bad address '" + std::string(fields[0]) + "'");
        }
        record.addr = *addr;
        if (fields[1] == "arm") {
            record.mode = analysis::Mode::Arm;
        } else if (fields[1] == "thumb") {
            record.mode = analysis::Mode::Thumb;
        } else {
            fail("mode must be arm or thumb, got '" + std::string(fields[1]) + "'");
        }
        if ((record.addr & (record.mode == analysis::Mode::Arm ? 3u : 1u)) != 0) {
            fail("misaligned address for its mode");
        }
        if (fields.size() == 3) {
            const auto rom = fields[2].starts_with("rom=") ? parse_hex(fields[2].substr(4)) : std::nullopt;
            if (!rom || !analysis::Image::is_rom(*rom)) {
                fail("expected rom=<ROM address>, got '" + std::string(fields[2]) + "'");
            }
            record.rom_src = *rom;
        }
        if (seen.insert(record.addr).second) {
            records.push_back(record);
        }
    }
    return records;
}

std::vector<MissRecord> read_miss_log(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw MissLogError("cannot open miss log: " + path.string());
    }
    std::ostringstream text;
    text << file.rdbuf();
    return parse_miss_log(text.str(), path.string());
}

std::string format_miss_record(const MissRecord& record) {
    char buf[64];
    const char* mode = record.mode == analysis::Mode::Arm ? "arm" : "thumb";
    if (record.rom_src) {
        std::snprintf(buf, sizeof buf, "0x%08x %s rom=0x%08x", record.addr, mode, *record.rom_src);
    } else {
        std::snprintf(buf, sizeof buf, "0x%08x %s", record.addr, mode);
    }
    return buf;
}

void apply_miss_log(const std::vector<MissRecord>& records, analysis::Image& image,
                    std::vector<analysis::EntryPoint>& entries) {
    for (const auto& record : records) {
        if (!image.contains(record.addr, 2) && record.rom_src) {
            const std::uint32_t ram_end = record.addr < 0x03000000 ? 0x02040000 : 0x03008000;
            const std::uint64_t rom_end = analysis::kRomBase + image.rom().size();
            const std::uint64_t size = std::min<std::uint64_t>(ram_end - record.addr, rom_end - *record.rom_src);
            image.add_region({*record.rom_src, record.addr, static_cast<std::uint32_t>(size)});
        }
        entries.push_back({record.addr, record.mode, analysis::EntrySource::MissLog});
    }
}

}  // namespace anygba::io
