#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#define ANYGBA_TEST_PID _getpid()
#else
#include <unistd.h>
#define ANYGBA_TEST_PID getpid()
#endif

namespace anygba::test {

// Named per process: two test runs at once (ctest -j next to a manual run) must not share files.
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name)
        : path(std::filesystem::temp_directory_path() /
               (std::string(name) + "-" + std::to_string(ANYGBA_TEST_PID))) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

}  // namespace anygba::test
