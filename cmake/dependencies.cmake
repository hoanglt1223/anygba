# Every dependency is pinned to an exact release and verified by SHA-256.
# Set FETCHCONTENT_SOURCE_DIR_<NAME> to build against a local checkout instead.
include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
  URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
FetchContent_MakeAvailable(SDL3)

if(ANYGBA_BUILD_TESTS)
  FetchContent_Declare(doctest
    URL https://raw.githubusercontent.com/doctest/doctest/v2.5.3/doctest/doctest.h
    URL_HASH SHA256=cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421
    DOWNLOAD_NO_EXTRACT TRUE)
  FetchContent_MakeAvailable(doctest)
  add_library(doctest INTERFACE)
  target_include_directories(doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")
  add_library(doctest::doctest ALIAS doctest)
endif()
