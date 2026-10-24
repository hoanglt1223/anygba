# Speed: paced headless runs at 200% and 400% take half and a quarter of real time, and the
# speed hotkeys step through the list.
#   cmake -DGAME=<exe> -DROM=<rom> -DWORK=<dir> -P player-speed.cmake
include("${CMAKE_CURRENT_LIST_DIR}/player-common.cmake")

# Shared CI runners stall unpredictably, so there only the lower bounds (pacing happens) are checked.
if(DEFINED ENV{CI})
  set(no_upper TRUE)
endif()

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
  if(seconds LESS low OR (NOT no_upper AND seconds GREATER high))
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

# Holding fast-forward runs at its set speed (paced, sound kept), not unthrottled: 240 frames at
# 200% take about 2 s. An input file sets 400%; --ff-speed overrides it for the run only.
file(WRITE "${WORK}/ff.ini" "[player]\nFastForwardSpeed = 400\n")
file(WRITE "${WORK}/hold.txt" "1 key down Space\n")
run_game(out --pace --ff-speed 200 --frames 240 --no-save --input "${WORK}/ff.ini" --script "${WORK}/hold.txt")
string(REGEX MATCH "stopped after 240 frames in ([0-9.]+) s" found "${out}")
if(NOT found OR CMAKE_MATCH_1 LESS 1.6 OR (NOT no_upper AND CMAKE_MATCH_1 GREATER 3.0))
  message(FATAL_ERROR "fast-forward at 200%: expected 1.6-3.0 s:\n${out}")
endif()
message(STATUS "fast-forward 200%: ${CMAKE_MATCH_1} s")
run_game(out --pace --frames 240 --no-save --input "${WORK}/ff.ini" --script "${WORK}/hold.txt")
string(REGEX MATCH "stopped after 240 frames in ([0-9.]+) s" found "${out}")
if(NOT found OR CMAKE_MATCH_1 LESS 0.8 OR (NOT no_upper AND CMAKE_MATCH_1 GREATER 1.6))
  message(FATAL_ERROR "fast-forward at 400% from the input file: expected 0.8-1.6 s:\n${out}")
endif()
message(STATUS "fast-forward 400%: ${CMAKE_MATCH_1} s")

# --wav records the sound as played: at 200% the 120 frames of sound (about 2.0 s at 32768 Hz)
# are squeezed into about 1.0 s, resampled, or time-stretched with NormalSpeedMusic.
file(WRITE "${WORK}/music.ini" "[player]\nNormalSpeedMusic = on\n")
foreach(case "resample;" "stretch;--input;${WORK}/music.ini")
  list(POP_FRONT case name)
  run_game(out --speed 200 --frames 120 --no-save --wav "${WORK}/${name}.wav" ${case})
  file(SIZE "${WORK}/${name}.wav" bytes)
  math(EXPR seconds_x100 "(${bytes} - 44) * 100 / (32768 * 4)")
  message(STATUS "--wav at 200% (${name}): ${bytes} bytes")
  if(seconds_x100 LESS 90 OR seconds_x100 GREATER 105)
    message(FATAL_ERROR "--wav at 200% (${name}): ${bytes} bytes, expected about 1.0 s of sound")
  endif()
  expect_bytes("${WORK}/${name}.wav" 0 "52494646")  # RIFF
  expect_bytes("${WORK}/${name}.wav" 8 "57415645")  # WAVE
endforeach()
