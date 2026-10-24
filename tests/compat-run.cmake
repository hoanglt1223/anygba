# Compatibility checkpoint for one compat/*.toml entry: runs the recompiled game headless for
# FRAMES frames and compares the last frame's hash (video only) with HASH.
#   cmake -DGAME=<exe> -DROM=<rom> -DFRAMES=<n> -DHASH=<sha256 or empty> -DWORK=<dir> -P compat-run.cmake
# With an empty HASH the test fails and prints the hash to record.
foreach(var GAME ROM FRAMES WORK)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()

file(MAKE_DIRECTORY "${WORK}")
file(REMOVE "${WORK}/miss.log")
execute_process(
  COMMAND "${GAME}" "${ROM}" --headless --frames ${FRAMES} --dump-frame-hash --no-save --miss-log "${WORK}/miss.log"
  WORKING_DIRECTORY "${WORK}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the game stopped with exit code ${rc}:\n${out}\n${err}")
endif()
string(REGEX MATCH "frame ([0-9]+) hash ([0-9a-f]+)" found "${out}")
if(NOT found)
  message(FATAL_ERROR "no frame hash in the output:\n${out}")
endif()
set(frame "${CMAKE_MATCH_1}")
set(actual "${CMAKE_MATCH_2}")
set(misses 0)
if(EXISTS "${WORK}/miss.log")
  file(STRINGS "${WORK}/miss.log" lines)
  list(LENGTH lines misses)
endif()
string(REGEX MATCH "\\(([0-9.]+) frames/s\\)" speed "${out}")
message(STATUS "frame ${frame} hash ${actual}; ${misses} interpreter misses; ${CMAKE_MATCH_1} frames/s unpaced")
if(NOT frame EQUAL FRAMES)
  message(FATAL_ERROR "stopped at frame ${frame}, expected ${FRAMES}")
endif()
if(NOT HASH STREQUAL actual)
  message(FATAL_ERROR "frame ${FRAMES} hash ${actual} differs from the recorded ${HASH}")
endif()
