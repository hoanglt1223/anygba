# Save states on a recompiled game: a state saved at a frame and loaded later (in the same run or
# a new one) continues exactly like the run that saved it.
#   cmake -DGAME=<exe> -DROM=<rom> -DVSYNC_GAME=<exe> -DVSYNC_ROM=<rom> -DWORK=<dir> -P player-states.cmake
include("${CMAKE_CURRENT_LIST_DIR}/player-common.cmake")
set(GAME "${VSYNC_GAME}")  # tonc swi_vsync: a sprite that turns every frame
set(ROM "${VSYNC_ROM}")

file(WRITE "${WORK}/save.txt" "120 key tap Shift+F1\n")
run_game(a --frames 400 --dump-frame-hash --save-dir "${WORK}/a" --script "${WORK}/save.txt")
expect_output("${a}" "osd: Saved state 1")
frame_hash(hash_a "${a}")
file(GLOB state "${WORK}/a/*.ss1")
if(NOT state)
  message(FATAL_ERROR "no .ss1 file in ${WORK}/a")
endif()

# A new process that starts from the state.
run_game(b --frames 400 --dump-frame-hash --save-dir "${WORK}/b" --load-state "${state}")
frame_hash(hash_b "${b}")

# Save, play on, load slot 1 again: the frame counter goes back and the same frames follow.
file(WRITE "${WORK}/save-load.txt" "120 key tap Shift+F1\n250 key tap F1\n")
run_game(c --frames 400 --dump-frame-hash --save-dir "${WORK}/c" --script "${WORK}/save-load.txt")
expect_output("${c}" "osd: Loaded state 1")
frame_hash(hash_c "${c}")

# An empty slot and a state from another game are reported, and the game goes on.
file(WRITE "${WORK}/empty.txt" "30 key tap F5\n")
run_game(d --frames 60 --save-dir "${WORK}/d" --script "${WORK}/empty.txt")
expect_output("${d}" "osd: No save state in slot 5")
file(GLOB stem_file "${WORK}/a/*.ss1")
get_filename_component(stem "${stem_file}" NAME_WE)
file(WRITE "${WORK}/e/${stem}.ss2" "not a state")
file(WRITE "${WORK}/bad.txt" "30 key tap F2\n")
run_game(e --frames 60 --save-dir "${WORK}/e" --script "${WORK}/bad.txt")
expect_output("${e}" "osd: Load state 2 failed: not a save state")

run_game(earlier --frames 399 --dump-frame-hash --no-save)
frame_hash(hash_earlier "${earlier}")
if(hash_earlier STREQUAL hash_a)
  message(FATAL_ERROR "the picture does not change between frames; the comparison would prove nothing")
endif()
message(STATUS "a ${hash_a}; b ${hash_b}; c ${hash_c}")
if(NOT hash_a STREQUAL hash_b OR NOT hash_a STREQUAL hash_c)
  message(FATAL_ERROR "frames after loading differ from the run that saved")
endif()
