#include "anygba/embedded_rom.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace anygba {

std::optional<std::filesystem::path> executable_path() {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::nullopt;
        if (n < buf.size()) {
            buf.resize(n);
            return std::filesystem::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::nullopt;
    buf.resize(std::strlen(buf.c_str()));
    return std::filesystem::path(buf);
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return std::nullopt;
    return p;
#endif
}

std::optional<std::vector<std::uint8_t>> read_appended_rom(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    const auto total = static_cast<std::uint64_t>(in.tellg());
    constexpr std::uint64_t kTrailer = 16;
    if (total < kTrailer) {
        return std::nullopt;
    }
    std::array<char, kTrailer> trailer{};
    in.seekg(static_cast<std::streamoff>(total - kTrailer));
    in.read(trailer.data(), kTrailer);
    if (!in || std::string_view(trailer.data() + 8, 8) != kEmbeddedRomMagic) {
        return std::nullopt;
    }
    std::uint64_t size = 0;
    for (int i = 7; i >= 0; --i) {
        size = (size << 8) | static_cast<unsigned char>(trailer[static_cast<std::size_t>(i)]);
    }
    if (size == 0 || size > 0x2000000 || size > total - kTrailer) {
        throw std::runtime_error("embedded ROM trailer in " + file.string() + " is damaged");
    }
    std::vector<std::uint8_t> rom(static_cast<std::size_t>(size));
    in.seekg(static_cast<std::streamoff>(total - kTrailer - size));
    in.read(reinterpret_cast<char*>(rom.data()), static_cast<std::streamsize>(size));
    if (!in) {
        throw std::runtime_error("cannot read the embedded ROM from " + file.string());
    }
    return rom;
}

void append_rom(const std::filesystem::path& file, std::span<const std::uint8_t> rom) {
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    const std::uint64_t size = rom.size();
    for (int i = 0; i < 8; ++i) {
        out.put(static_cast<char>((size >> (8 * i)) & 0xFF));
    }
    out.write(kEmbeddedRomMagic.data(), static_cast<std::streamsize>(kEmbeddedRomMagic.size()));
    out.close();
    if (!out) {
        throw std::runtime_error("cannot append the ROM to " + file.string());
    }
}

std::optional<std::vector<std::uint8_t>> read_embedded_rom() {
    const auto exe = executable_path();
    if (!exe) {
        return std::nullopt;
    }
    if (auto rom = read_appended_rom(*exe)) {
        return rom;
    }
    const auto bundled = exe->parent_path().parent_path() / "Resources" / "game.gba";
    std::ifstream in(bundled, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace anygba
