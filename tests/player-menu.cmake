# The in-game menu driven by scripted keys: cheat database picks with values, a typed code,
# rebinding; then everything must persist into the next run. Also a gamepad session.
#   cmake -DGAME=<exe> -DROM=<rom> -DDATA=<tests/player> -DWORK=<dir> -P player-menu.cmake
include("${CMAKE_CURRENT_LIST_DIR}/player-common.cmake")

run_game(ignored --write-input "${WORK}/input.ini")
run_game(out --frames 200 --save-dir "${WORK}/saves" --input "${WORK}/input.ini"
  --cheat-db "${DATA}/test-cheats.toml" --script "${DATA}/menu-script.txt" --dump "ewram:${WORK}/ewram.bin")
expect_output("${out}" "menu\\| == Cheats ==")
expect_output("${out}" "menu\\| == Pick an item: Item ==")
expect_output("${out}" "Added Code 1 \\(CodeBreaker\\)")
expect_output("${out}" "menu\\| \\(closed\\)")
expect_bytes("${WORK}/ewram.bin" 258048 "bebafeca")   # Write marker
expect_bytes("${WORK}/ewram.bin" 258064 "42")         # Byte marker
expect_bytes("${WORK}/ewram.bin" 258080 "10000c00")   # Elixir (0x10), quantity 12
expect_bytes("${WORK}/ewram.bin" 258096 "77")         # the typed code

file(GLOB cheat_file "${WORK}/saves/*.cheats.toml")
file(READ "${cheat_file}" cheats)
foreach(expected "'Items/Pick an item' = 16" "'Items/Quantity' = 12" "'Memory/Write marker' = true"
                 "name = 'Code 1'")
  string(FIND "${cheats}" "${expected}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "cheat file lacks ${expected}:\n${cheats}")
  endif()
endforeach()
file(READ "${WORK}/input.ini" ini)
if(NOT ini MATCHES "\\[keyboard\\]\nA = C\n" OR NOT ini MATCHES "\\[gamepad\\]\nA = South\nB = North\n")
  message(FATAL_ERROR "rebinding was not saved:\n${ini}")
endif()

# The next run starts with the same cheats on.
run_game(again --frames 60 --save-dir "${WORK}/saves" --input "${WORK}/input.ini"
  --cheat-db "${DATA}/test-cheats.toml" --dump "ewram:${WORK}/ewram2.bin")
expect_output("${again}" "osd: 5 cheats on")
expect_bytes("${WORK}/ewram2.bin" 258080 "10000c00")
expect_bytes("${WORK}/ewram2.bin" 258096 "77")

# A gamepad connects, Back+Start opens the menu, the stick moves, B closes.
run_game(pad --frames 120 --no-save --script "${DATA}/gamepad-script.txt")
expect_output("${pad}" "osd: Gamepad connected: Test Pad")
expect_output("${pad}" "menu\\| > Save state")
expect_output("${pad}" "menu\\| \\(closed\\)")
