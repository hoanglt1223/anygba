# Every dependency is pinned to an exact release and verified by SHA-256.
# Set FETCHCONTENT_SOURCE_DIR_<NAME> to build against a local checkout instead.
include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
# Installed with the runtime so generated games can link it from a release package.
set(SDL_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
  URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
FetchContent_MakeAvailable(SDL3)

# toml++ (MIT): per-game config parsing in the recompiler. Header-only.
FetchContent_Declare(tomlplusplus
  URL https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp
  URL_HASH SHA256=6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(tomlplusplus)

# ONNX Runtime C API headers (MIT) for AI upscaling. Only the headers: the library is loaded at
# run time when a player turns AI upscaling on (docs/user/HD-PACKS.md).
FetchContent_Declare(onnxruntime_api
  URL https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/include/onnxruntime/core/session/onnxruntime_c_api.h
  URL_HASH SHA256=9ed0d7054a4e74249467365b25b415d36f51a44a6349e2a994a1812e4723d1e2
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_Declare(onnxruntime_ep_api
  URL https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/include/onnxruntime/core/session/onnxruntime_ep_c_api.h
  URL_HASH SHA256=e94ac3490697bdb107844fa9d403952ee7136134132a057eae5c1207b57ce352
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(onnxruntime_api onnxruntime_ep_api)
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus SYSTEM INTERFACE "${tomlplusplus_SOURCE_DIR}")
add_library(tomlplusplus::tomlplusplus ALIAS tomlplusplus)

if(ANYGBA_BUILD_TESTS)
  # Capstone (BSD-3-Clause) is a test oracle for the disassembler only; nothing shipped links it.
  set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_ARM_SUPPORT ON CACHE BOOL "" FORCE)
  set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(capstone
    URL https://github.com/capstone-engine/capstone/archive/refs/tags/5.0.6.tar.gz
    URL_HASH SHA256=240ebc834c51aae41ca9215d3190cc372fd132b9c5c8aa2d5f19ca0c325e28f9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    SYSTEM)
  FetchContent_MakeAvailable(capstone)

  FetchContent_Declare(doctest
    URL https://raw.githubusercontent.com/doctest/doctest/v2.5.3/doctest/doctest.h
    URL_HASH SHA256=cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421
    DOWNLOAD_NO_EXTRACT TRUE)
  FetchContent_MakeAvailable(doctest)
  add_library(doctest INTERFACE)
  target_include_directories(doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")
  add_library(doctest::doctest ALIAS doctest)
endif()
