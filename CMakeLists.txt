cmake_minimum_required(VERSION 3.28)

project(anygba
  VERSION 0.1.0
  DESCRIPTION "Static recompiler that turns GBA ROMs into native executables"
  LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(ANYGBA_BUILD_TESTS "Build unit tests and the platform smoke app" ${PROJECT_IS_TOP_LEVEL})

include(toolchain)
include(warnings)
include(sanitizers)
include(dependencies)
include(anygba_game)

add_subdirectory(common)
add_subdirectory(platform)
add_subdirectory(runtime)
add_subdirectory(tools/recomp)
# The build driver runs on the PC only; an Android build is the runtime package alone.
if(NOT ANDROID)
  add_subdirectory(tools/anygba)
  add_subdirectory(tools/mcp)
endif()

if(ANYGBA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(install)
