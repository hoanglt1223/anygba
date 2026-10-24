# Trace-feedback check, run by CTest as a script (cmake -P): recompile ROM, build it against
# the installed runtime, run it headless and collect the miss log; then recompile with every
# log so far and run again, until a run has no misses. Each pass can only reach code the
# previous one ran (switch cases behind unbounded jumps, for instance, appear one level at a
# time), so up to MAX_PASSES (default 4) are allowed.
#
# Inputs (-D): RECOMP, PREFIX, ROM, WORK, FRAMES, CONFIG, GENERATOR, GENERATOR_PLATFORM, MAX_PASSES
foreach(var RECOMP PREFIX ROM WORK FRAMES CONFIG GENERATOR)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "feedback-pass.cmake needs -D${var}=...")
  endif()
endforeach()

set(generator_args -G "${GENERATOR}")
if(GENERATOR_PLATFORM)
  list(APPEND generator_args -A "${GENERATOR_PLATFORM}")
endif()

function(run_checked)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "command failed (${rc}): ${ARGN}\n${out}\n${err}")
  endif()
endfunction()

# Recompiles (with optional miss logs), builds, runs; leaves the miss log in ${WORK}/${tag}.log.
function(pass tag)
  set(gen "${WORK}/${tag}")
  file(REMOVE_RECURSE "${gen}")
  file(REMOVE "${WORK}/${tag}.log")
  set(logs "")
  foreach(log IN LISTS ARGN)
    list(APPEND logs --miss-log "${log}")
  endforeach()
  run_checked("${RECOMP}" "${ROM}" --output "${gen}" ${logs})
  run_checked("${CMAKE_COMMAND}" -S "${gen}" -B "${gen}/out" ${generator_args}
              "-DCMAKE_PREFIX_PATH=${PREFIX}" "-DCMAKE_BUILD_TYPE=${CONFIG}")
  run_checked("${CMAKE_COMMAND}" --build "${gen}/out" --config "${CONFIG}")
  file(GLOB_RECURSE game LIST_DIRECTORIES false "${gen}/out/game" "${gen}/out/game.exe")
  if(NOT game)
    message(FATAL_ERROR "no game executable under ${gen}/out")
  endif()
  list(GET game 0 game)
  run_checked("${game}" "${ROM}" --headless --frames ${FRAMES} --miss-log "${WORK}/${tag}.log")
endfunction()

file(MAKE_DIRECTORY "${WORK}")
if(NOT MAX_PASSES)
  set(MAX_PASSES 4)
endif()
set(logs "")
foreach(n RANGE 1 ${MAX_PASSES})
  pass(pass${n} ${logs})
  set(missed "")
  if(EXISTS "${WORK}/pass${n}.log")
    file(READ "${WORK}/pass${n}.log" missed)
  endif()
  string(STRIP "${missed}" missed)
  if(missed STREQUAL "")
    math(EXPR feedback "${n} - 1")
    message(STATUS "no misses after ${feedback} feedback pass(es)")
    return()
  endif()
  message(STATUS "pass ${n} missed:\n${missed}")
  list(APPEND logs "${WORK}/pass${n}.log")
endforeach()
message(FATAL_ERROR "misses remain after ${MAX_PASSES} passes")
