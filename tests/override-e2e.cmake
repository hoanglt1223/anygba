# Function overrides end to end on tonc's irq_demo, built with tests/overrides/call-paths: every call
# path reaches the overrides, --overrides off runs the original code, and the picture is the same.
#   cmake -DGAME=<exe> -DROM=<rom> -DWORK=<dir> -P override-e2e.cmake
foreach(var GAME ROM WORK)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()
file(MAKE_DIRECTORY "${WORK}")

function(run_game mode out_var)
  execute_process(
    COMMAND "${GAME}" "${ROM}" --headless --frames 600 --dump-frame-hash --no-save --no-miss-log --overrides ${mode}
    WORKING_DIRECTORY "${WORK}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "--overrides ${mode}: the game stopped with exit code ${rc}:\n${out}\n${err}")
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

function(frame_hash out text)
  string(REGEX MATCH "frame 600 hash ([0-9a-f]+)" found "${text}")
  if(NOT found)
    message(FATAL_ERROR "no frame 600 hash in the output:\n${text}")
  endif()
  set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(expect text regex what)
  if(NOT text MATCHES "${regex}")
    message(FATAL_ERROR "${what}: no match for '${regex}' in:\n${text}")
  endif()
endfunction()

run_game(on with)
expect("${with}" "override: 0x08004104 thumb -> call_paths_direct \\[test\\]: on" "active overrides listed")
expect("${with}" "call-paths direct: recompiled=[1-9][0-9]* other=0" "bl from recompiled code")
expect("${with}" "call-paths indirect: recompiled=[1-9][0-9]* other=0" "bx from recompiled code (isr_master)")
expect("${with}" "call-paths interpreted: recompiled=0 other=[1-9]" "bl from interpreted code")

run_game(off without)
expect("${without}" "call_paths_direct \\[test\\]: off\n" "overrides listed as off")
foreach(path direct indirect interpreted)
  expect("${without}" "call-paths ${path}: recompiled=0 other=0" "--overrides off reaches no override")
endforeach()

frame_hash(hash_with "${with}")
frame_hash(hash_without "${without}")
message(STATUS "frame 600 hash ${hash_with} with the overrides, ${hash_without} without")
if(NOT hash_with STREQUAL hash_without)
  message(FATAL_ERROR "the overrides changed the picture")
endif()
