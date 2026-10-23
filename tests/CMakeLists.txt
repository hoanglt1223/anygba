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
  support/elf_symbols.cpp
  support/corpus.cpp
  runtime/runtime_tests.cpp)
target_link_libraries(anygba-tests PRIVATE
  anyport::platform
  anygba::runtime
  anygba_recomp_lib
  capstone_static
  doctest::doctest)
target_include_directories(anygba-tests PRIVATE .)
# Corpus-backed tests read from here and report themselves skipped when it is missing.
target_compile_definitions(anygba-tests PRIVATE ANYGBA_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
anygba_configure_target(anygba-tests)

add_test(NAME unit-tests COMMAND anygba-tests)

add_executable(platform-smoke platform-smoke/main.cpp)
target_link_libraries(platform-smoke PRIVATE anyport::platform)
anygba_configure_target(platform-smoke)

add_test(NAME platform-smoke-headless COMMAND platform-smoke --headless --frames 60 --scale 2)
