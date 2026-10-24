# Helpers for the player tests (player-*.cmake): run a generated game headless and read its output.

foreach(var GAME ROM WORK)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()

# run_game(<out var> args...): runs GAME ROM --headless --no-miss-log args...; fails on a non-zero exit.
function(run_game out_var)
  execute_process(
    COMMAND "${GAME}" "${ROM}" --headless --no-miss-log ${ARGN}
    WORKING_DIRECTORY "${WORK}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "game ${ARGN} exited with ${rc}:\n${out}\n${err}")
  endif()
  set(${out_var} "${out}${err}" PARENT_SCOPE)
endfunction()

function(frame_hash out_var text)
  string(REGEX MATCH "frame ([0-9]+) hash ([0-9a-f]+)" found "${text}")
  if(NOT found)
    message(FATAL_ERROR "no frame hash in:\n${text}")
  endif()
  set(${out_var} "${CMAKE_MATCH_1}:${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

function(expect_output text regex)
  if(NOT text MATCHES "${regex}")
    message(FATAL_ERROR "expected output matching '${regex}' in:\n${text}")
  endif()
endfunction()

# expect_bytes(<file> <offset> <hex>): the bytes at offset must be `hex` (lower case).
function(expect_bytes file offset hex)
  string(LENGTH "${hex}" digits)
  math(EXPR count "${digits} / 2")
  file(READ "${file}" actual OFFSET ${offset} LIMIT ${count} HEX)
  if(NOT actual STREQUAL hex)
    message(FATAL_ERROR "${file} at ${offset}: expected ${hex}, got ${actual}")
  endif()
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
