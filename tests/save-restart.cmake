# Runs a recompiled jsmolka save test twice against one save folder: both runs must pass
# (r12 = 0), the save file must have the chip's size after the first run, and the second
# run must load it (it reports the save path).
#   cmake -DGAME=<exe> -DROM=<rom> -DDIR=<save dir> -DSIZE=<bytes, 0 = no file> -P save-restart.cmake
foreach(var GAME ROM DIR SIZE)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${DIR}")
foreach(run 1 2)
  execute_process(
    COMMAND "${GAME}" "${ROM}" --headless --exit-on-idle --frames 600 --no-miss-log --save-dir "${DIR}"
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0 OR NOT out MATCHES "r12=00000000")
    message(FATAL_ERROR "run ${run} failed (exit ${rc}):\n${out}\n${err}")
  endif()
  file(GLOB saves "${DIR}/*.sav")
  list(LENGTH saves count)
  if(SIZE EQUAL 0)
    if(NOT count EQUAL 0)
      message(FATAL_ERROR "a game without backup memory wrote ${saves}")
    endif()
    continue()
  endif()
  if(NOT count EQUAL 1)
    message(FATAL_ERROR "run ${run}: expected one save file in ${DIR}, found '${saves}'")
  endif()
  file(SIZE "${saves}" bytes)
  if(NOT bytes EQUAL SIZE)
    message(FATAL_ERROR "run ${run}: ${saves} is ${bytes} bytes, expected ${SIZE}")
  endif()
  if(NOT out MATCHES "save: ")
    message(FATAL_ERROR "run ${run} did not report its save file:\n${out}")
  endif()
endforeach()
message(STATUS "save kept across restarts: ${saves}")
