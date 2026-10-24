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
  support/png.cpp
  support/headless_run.cpp
  support/test_roms.cpp
  runtime/runtime_tests.cpp
  runtime/interpreter_tests.cpp
  runtime/system_tests.cpp
  runtime/bios_tests.cpp
  runtime/ppu_tests.cpp
  runtime/apu_tests.cpp
  runtime/save_tests.cpp
  driver/driver_tests.cpp)
target_link_libraries(anygba-tests PRIVATE
  anyport::platform
  anygba::runtime
  anygba::common
  anygba_recomp_lib
  anygba_driver_lib
  capstone_static
  anygba_codegen_fixture
  doctest::doctest)
target_include_directories(anygba-tests PRIVATE . "${PROJECT_SOURCE_DIR}/runtime/src")
# Corpus-backed tests read from here and report themselves skipped when it is missing.
set(corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
target_compile_definitions(anygba-tests PRIVATE ANYGBA_CORPUS_DIR="${corpus}"
  ANYGBA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime/data"
  ANYGBA_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
anygba_configure_target(anygba-tests)

add_test(NAME unit-tests COMMAND anygba-tests)

# Headless interpreter runs with screenshots and frame hashes, mirroring tests/oracle/mgba-oracle.py.
add_executable(anygba-shot support/shot_main.cpp support/corpus.cpp support/png.cpp support/headless_run.cpp)
target_link_libraries(anygba-shot PRIVATE anygba::runtime anygba::common)
target_include_directories(anygba-shot PRIVATE .)
target_compile_definitions(anygba-shot PRIVATE ANYGBA_CORPUS_DIR="${corpus}")
anygba_configure_target(anygba-shot)

add_executable(anygba-test-roms support/test_roms_main.cpp support/test_roms.cpp)
target_include_directories(anygba-test-roms PRIVATE .)
anygba_configure_target(anygba-test-roms)

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
    COMMAND ${name} "${corpus}/${rom}" --headless --exit-on-idle --frames 600 --no-miss-log
      --save-dir "${CMAKE_CURRENT_BINARY_DIR}/saves")
  set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "r12=00000000")
endfunction()

anygba_rom_test(gba-tests-arm gba-tests/arm/arm.gba)
anygba_rom_test(gba-tests-thumb gba-tests/thumb/thumb.gba)
anygba_rom_test(gba-tests-memory gba-tests/memory/memory.gba)
anygba_rom_test(gba-tests-bios gba-tests/bios/bios.gba)

# jsmolka save tests, each run twice against one save folder.
foreach(save sram:32768 flash64:65536 flash128:131072 none:0)
  string(REPLACE ":" ";" parts "${save}")
  list(GET parts 0 kind)
  list(GET parts 1 size)
  set(rom "gba-tests/save/${kind}.gba")
  if(EXISTS "${corpus}/${rom}")
    anygba_add_game(gba-tests-save-${kind} ROM "${corpus}/${rom}" SHARDS 1)
    add_test(NAME gba-tests-save-${kind}
      COMMAND "${CMAKE_COMMAND}" "-DGAME=$<TARGET_FILE:gba-tests-save-${kind}>" "-DROM=${corpus}/${rom}"
        "-DDIR=${CMAKE_CURRENT_BINARY_DIR}/saves/${kind}" -DSIZE=${size}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/save-restart.cmake")
  endif()
endforeach()

# The committed compat data must render (schema checks included).
find_package(Python3 3.11 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME compat-site
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/compat-site/generate.py"
      --out "${CMAKE_CURRENT_BINARY_DIR}/compat-site")
endif()

# Compatibility checkpoints: every compat/*.toml entry with a corpus ROM is recompiled and run
# headless; the last frame's hash must match the recorded one (compat/schema.md).
option(ANYGBA_COMPAT_TESTS "Build the compat/*.toml titles and check their frame-hash checkpoints" OFF)
if(ANYGBA_COMPAT_TESTS)
  file(GLOB compat_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/compat/*.toml")
  foreach(compat_file IN LISTS compat_files)
    get_filename_component(id "${compat_file}" NAME_WE)
    foreach(key rom checkpoint_frames checkpoint_hash shards)
      file(STRINGS "${compat_file}" line REGEX "^${key} = ")
      string(REGEX REPLACE "^${key} = \"?([^\"]*)\"?.*$" "\\1" value "${line}")
      set(compat_${key} "${value}")
    endforeach()
    if(NOT compat_rom OR NOT compat_checkpoint_frames)
      continue()  # commercial titles: no corpus ROM, tracked by hand
    endif()
    if(NOT EXISTS "${corpus}/${compat_rom}")
      message(STATUS "Corpus ROM ${compat_rom} not fetched; skipping compat-${id}")
      continue()
    endif()
    if(NOT compat_shards)
      set(compat_shards 2)
    endif()
    anygba_add_game(compat-${id} ROM "${corpus}/${compat_rom}" SHARDS ${compat_shards})
    add_test(NAME compat-${id}
      COMMAND "${CMAKE_COMMAND}" "-DGAME=$<TARGET_FILE:compat-${id}>" "-DROM=${corpus}/${compat_rom}"
        -DFRAMES=${compat_checkpoint_frames} "-DHASH=${compat_checkpoint_hash}"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/compat/${id}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/compat-run.cmake")
    set_tests_properties(compat-${id} PROPERTIES LABELS compat TIMEOUT 600)
  endforeach()
endif()

# A large real game, compiled on demand to keep the generated C honest:
#   cmake --build <out> --target varooom-3d
if(EXISTS "${corpus}/butano/varooom-3d/varooom-3d.gba")
  anygba_add_game(varooom-3d ROM "${corpus}/butano/varooom-3d/varooom-3d.gba" SHARDS 8)
  set_target_properties(varooom-3d PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# tonc demos used by the runtime validation (frame count, trace feedback), built on demand.
foreach(demo irq_demo dma_demo swi_vsync tmr_demo swi_demo snd1_demo)
  if(EXISTS "${corpus}/tonc/bin/${demo}.gba")
    anygba_add_game(tonc-${demo} ROM "${corpus}/tonc/bin/${demo}.gba" SHARDS 2)
    set_target_properties(tonc-${demo} PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()
endforeach()

# Trace feedback, end to end through an installed runtime: after one recompile with the
# miss log of a first run, a second run must not need the interpreter.
set(test_prefix "${CMAKE_BINARY_DIR}/test-prefix")
add_test(NAME install-runtime
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --config $<CONFIG> --prefix "${test_prefix}")
set_tests_properties(install-runtime PROPERTIES FIXTURES_SETUP runtime_install)
if(EXISTS "${corpus}/gba-tests/save/sram.gba")
  add_test(NAME driver-build
    COMMAND "${CMAKE_COMMAND}"
      "-DDRIVER=$<TARGET_FILE:anygba>" "-DPREFIX=${test_prefix}" "-DROM=${corpus}/gba-tests/save/sram.gba"
      "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/driver-build" -DCONFIG=$<CONFIG> "-DGENERATOR=${CMAKE_GENERATOR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/driver-build.cmake")
  set_tests_properties(driver-build PROPERTIES FIXTURES_REQUIRED runtime_install LABELS slow TIMEOUT 900)
endif()
foreach(demo irq_demo dma_demo)
  if(EXISTS "${corpus}/tonc/bin/${demo}.gba")
    add_test(NAME feedback-${demo}
      COMMAND "${CMAKE_COMMAND}"
        "-DRECOMP=$<TARGET_FILE:anygba-recomp>" "-DPREFIX=${test_prefix}"
        "-DROM=${corpus}/tonc/bin/${demo}.gba" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/feedback-${demo}"
        -DFRAMES=600 -DCONFIG=$<CONFIG> "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/feedback-pass.cmake")
    set_tests_properties(feedback-${demo} PROPERTIES FIXTURES_REQUIRED runtime_install LABELS slow TIMEOUT 900)
  endif()
endforeach()
