// anygba-test-roms: writes the self-authored test ROMs (support/test_roms.cpp) into a
// directory, so tests/oracle scripts can run exactly the same programs in mGBA.
#include "support/test_roms.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("usage: anygba-test-roms <output directory>\n", stderr);
        return 2;
    }
    try {
        const std::filesystem::path dir = argv[1];
        std::filesystem::create_directories(dir);
        for (const auto& r : anygba::test::apu_test_roms()) {
            std::ofstream f(dir / ("apu-" + r.name + ".gba"), std::ios::binary);
            f.write(reinterpret_cast<const char*>(r.rom.data()), static_cast<std::streamsize>(r.rom.size()));
            if (!f) {
                std::fprintf(stderr, "cannot write %s\n", r.name.c_str());
                return 1;
            }
            std::printf("apu-%s.gba\n", r.name.c_str());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
