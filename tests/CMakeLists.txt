# Differential-test fixture: generated C for sampled instructions, written by the build.
add_executable(anygba-codegen-fixture support/codegen_fixture_main.cpp)
target_link_libraries(anygba-codegen-fixture PRIVATE anygba_recomp_lib)
anygba_configure_target(anygba-codegen-fixture)
set(codegen_fixture "${CMAKE_CURRENT_BINARY_DIR}/codegen_fixture.c")
add_custom_command(
  OUTPUT "${codegen_fixture}"
  COMMAND anygba-codegen-fixture "${codegen_fixture}"
  DEPENDS anygba-codegen-fixture
  COMMENT "Generating the codegen differential fixture"
  VERBATIM)
# Generated code is built like a generated game: compiler defaults, not the project's warnings.
add_library(anygba_codegen_fixture STATIC "${codegen_fixture}")
target_include_directories(anygba_codegen_fixture PRIVATE .)
target_link_libraries(anygba_codegen_fixture PRIVATE anygba::runtime)

add_executable(anygba-tests
  test_main.cpp
  platform/input_tests.cpp
  platform/window_tests.cpp
  platform/audio_tests.cpp
  platform/timing_tests.cpp
  recomp/cli_tests.cpp
  recomp/decode_tests.cpp
  recomp/discovery_tests.cpp
  recomp/config_tests.cpp
  recomp/codegen_tests.cpp
  support/elf_symbols.cpp
  support/corpus.cpp
  runtime/runtime_tests.cpp
  runtime/interpreter_tests.cpp)
target_link_libraries(anygba-tests PRIVATE
  anyport::platform
  anygba::runtime
  anygba_recomp_lib
  capstone_static
  anygba_codegen_fixture
  doctest::doctest)
target_include_directories(anygba-tests PRIVATE . "${PROJECT_SOURCE_DIR}/runtime/src")
# Corpus-backed tests read from here and report themselves skipped when it is missing.
set(corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
target_compile_definitions(anygba-tests PRIVATE ANYGBA_CORPUS_DIR="${corpus}")
anygba_configure_target(anygba-tests)

add_test(NAME unit-tests COMMAND anygba-tests)

add_executable(platform-smoke platform-smoke/main.cpp)
target_link_libraries(platform-smoke PRIVATE anyport::platform)
anygba_configure_target(platform-smoke)

add_test(NAME platform-smoke-headless COMMAND platform-smoke --headless --frames 60 --scale 2)

# Recompiled corpus ROMs. jsmolka's tests end in `idle: b idle` with the number of the
# first failing test in r12 (0 = all passed).
function(anygba_rom_test name rom)
  if(NOT EXISTS "${corpus}/${rom}")
    message(STATUS "Corpus ROM ${rom} not fetched; skipping ${name} (run tests/fetch-corpus)")
    return()
  endif()
  anygba_add_game(${name} ROM "${corpus}/${rom}" SHARDS 2)
  add_test(NAME ${name}
    COMMAND ${name} "${corpus}/${rom}" --headless --exit-on-idle --frames 600 --no-miss-log)
  set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "r12=00000000")
endfunction()

anygba_rom_test(gba-tests-arm gba-tests/arm/arm.gba)
anygba_rom_test(gba-tests-thumb gba-tests/thumb/thumb.gba)
anygba_rom_test(gba-tests-memory gba-tests/memory/memory.gba)

# A large real game, compiled on demand to keep the generated C honest:
#   cmake --build <out> --target varooom-3d
if(EXISTS "${corpus}/butano/varooom-3d/varooom-3d.gba")
  anygba_add_game(varooom-3d ROM "${corpus}/butano/varooom-3d/varooom-3d.gba" SHARDS 8)
  set_target_properties(varooom-3d PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
