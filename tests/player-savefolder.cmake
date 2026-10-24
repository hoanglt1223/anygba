# The save folder setting: SaveFolder in the input file (a path with spaces, like OneDrive's
# "My Games") holds save states and the cheat file; --save-dir still overrides it; a relative
# SaveFolder is taken from the input file's folder.
#   cmake -DGAME=<exe> -DROM=<rom> -DWORK=<dir> -P player-savefolder.cmake
include("${CMAKE_CURRENT_LIST_DIR}/player-common.cmake")

set(folder "${WORK}/OneDrive/Documents/My Games/AnyGBA/saves")
file(WRITE "${WORK}/input.ini" "[player]\nSaveFolder = ${folder}\n")
file(WRITE "${WORK}/save.txt" "10 key tap Shift+F1\n")
run_game(out --frames 30 --input "${WORK}/input.ini" --script "${WORK}/save.txt")
expect_output("${out}" "osd: Saved state 1")
file(GLOB states "${folder}/*.ss1")
if(NOT states)
  message(FATAL_ERROR "no save state in the SaveFolder ${folder}:\n${out}")
endif()

run_game(out --frames 30 --input "${WORK}/input.ini" --save-dir "${WORK}/override" --script "${WORK}/save.txt")
file(GLOB states "${WORK}/override/*.ss1")
if(NOT states)
  message(FATAL_ERROR "--save-dir did not override SaveFolder:\n${out}")
endif()

file(MAKE_DIRECTORY "${WORK}/portable")
file(WRITE "${WORK}/portable/anygba-input.ini" "[player]\nSaveFolder = my saves\n")
run_game(out --frames 30 --input "${WORK}/portable/anygba-input.ini" --script "${WORK}/save.txt")
file(GLOB states "${WORK}/portable/my saves/*.ss1")
if(NOT states)
  message(FATAL_ERROR "a relative SaveFolder is not next to the input file:\n${out}")
endif()

# Loading from the folder in a later run.
file(WRITE "${WORK}/load.txt" "10 key tap F1\n")
run_game(out --frames 30 --input "${WORK}/input.ini" --script "${WORK}/load.txt")
expect_output("${out}" "osd: Loaded state 1")
