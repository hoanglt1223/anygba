# The stats overlay: Ctrl+F turns it on, headless runs print its lines as "stats| ...", the
# setting is saved to the input file and the next run starts with it on.
#   cmake -DGAME=<exe> -DROM=<rom> -DWORK=<dir> -P player-stats.cmake
include("${CMAKE_CURRENT_LIST_DIR}/player-common.cmake")

run_game(ignored --write-input "${WORK}/input.ini")
file(WRITE "${WORK}/toggle.txt" "5 key tap Ctrl+F\n")
run_game(out --pace --frames 150 --no-save --input "${WORK}/input.ini" --script "${WORK}/toggle.txt")
expect_output("${out}" "osd: Stats on")
expect_output("${out}" "stats\\| FPS [0-9.]+ host, [0-9.]+ GBA")
expect_output("${out}" "stats\\| Speed [0-9]+% of 100%")
expect_output("${out}" "stats\\| Frame [0-9.]+ ms")
expect_output("${out}" "stats\\| Audio off, 0 underruns")
expect_output("${out}" "stats\\| Interpreter [0-9.]+%")
expect_output("${out}" "stats\\| Video nearest headless")
expect_output("${out}" "stats\\| Cheats 0  Save [a-z0-9]+  RTC (on|off)")
# Paced at 100%: the measured rates are near 59.7 frames per second.
string(REGEX MATCH "stats\\| FPS ([0-9.]+) host, ([0-9.]+) GBA" found "${out}")
foreach(rate "${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
  if(rate LESS 50 OR rate GREATER 70)
    message(FATAL_ERROR "stats rate ${rate} is not near 59.7:\n${out}")
  endif()
endforeach()
string(REGEX MATCH "stats\\| Interpreter ([0-9.]+)%" found "${out}")
if(CMAKE_MATCH_1 GREATER 100)
  message(FATAL_ERROR "interpreter share above 100%:\n${out}")
endif()
file(READ "${WORK}/input.ini" ini)
expect_output("${ini}" "Stats = on")

# On from the start; Ctrl+F turns it off and saves that.
run_game(out --frames 150 --pace --no-save --input "${WORK}/input.ini" --script "${WORK}/toggle.txt")
expect_output("${out}" "osd: Stats off")
file(READ "${WORK}/input.ini" ini)
expect_output("${ini}" "Stats = off")
run_game(out --pace --frames 90 --no-save --input "${WORK}/input.ini")
if(out MATCHES "stats\\|")
  message(FATAL_ERROR "stats printed while off:\n${out}")
endif()
