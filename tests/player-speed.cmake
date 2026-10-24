# Speed: paced headless runs at 200% and 400% take half and a quarter of real time, and the
# speed hotkeys step through the list.
#   cmake -DGAME=<exe> -DROM=<rom> -DWORK=<dir> -P player-speed.cmake
include("${CMAKE_CURRENT_LIST_DIR}/player-common.cmake")

# 240 frames at 59.73 Hz: 4.02 s at 100%, 2.01 s at 200%, 1.00 s at 400%.
foreach(case "200;1.6;3.0" "400;0.8;1.6")
  list(GET case 0 speed)
  list(GET case 1 low)
  list(GET case 2 high)
  run_game(out --pace --speed ${speed} --frames 240 --no-save)
  string(REGEX MATCH "stopped after 240 frames in ([0-9.]+) s" found "${out}")
  if(NOT found)
    message(FATAL_ERROR "no timing in:\n${out}")
  endif()
  set(seconds "${CMAKE_MATCH_1}")
  message(STATUS "speed ${speed}%: ${seconds} s")
  if(seconds LESS low OR seconds GREATER high)
    message(FATAL_ERROR "speed ${speed}%: 240 frames took ${seconds} s, expected ${low}-${high} s")
  endif()
endforeach()

file(WRITE "${WORK}/keys.txt" "10 key tap =\n20 key tap =\n30 key tap =\n40 key tap =\n50 key tap -\n")
run_game(out --frames 60 --no-save --script "${WORK}/keys.txt")
foreach(expected "Speed 200%" "Speed 300%" "Speed 400%")
  expect_output("${out}" "osd: ${expected}")
endforeach()
string(REGEX MATCHALL "osd: Speed 400%" fours "${out}")
list(LENGTH fours count)
if(NOT count EQUAL 2)
  message(FATAL_ERROR "the speed should stay at 400% when stepped past it:\n${out}")
endif()
