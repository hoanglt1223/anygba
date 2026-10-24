# Cheat codes on recompiled games, from the command line: every device format, encrypted
# forms, button conditions, and a ROM patch on code the recompiler compiled.
#   cmake -DGAME=<irq_demo exe> -DROM=<rom> -DFIRST_GAME=<first exe> -DFIRST_ROM=<rom> -DWORK=<dir>
#         -P player-cheats.cmake
include("${CMAKE_CURRENT_LIST_DIR}/player-common.cmake")

run_game(out --frames 120 --no-save --keys a:5-200 --dump "ewram:${WORK}/ewram.bin"
  --cheat "raw:0203F000:CAFEBABE"
  --cheat "cb:3203F010 0042"
  # 0423F020 11223344 (32-bit write) and 2203F030 55667788, encrypted as the devices print them
  --cheat "arv3:88DDF218 EF4FC025"
  --cheat "gsv1:51981EED D6E465D2"
  # CodeBreaker joypad conditions: A is held, B is not
  --cheat "cb:D0000020 0001,8203F040 1234"
  --cheat "cb:D0000020 0002,8203F042 1234"
  # Auto-detected Action Replay v3 master code (Emerald's): enable codes only
  --cheat "A86CDBA5 19BA49B3,D8BAE4D9 4864DCE5")
expect_bytes("${WORK}/ewram.bin" 258048 "bebafeca")    # 0x3F000
expect_bytes("${WORK}/ewram.bin" 258064 "42")          # 0x3F010
expect_bytes("${WORK}/ewram.bin" 258080 "44332211")    # 0x3F020
expect_bytes("${WORK}/ewram.bin" 258096 "88776655")    # 0x3F030
expect_bytes("${WORK}/ewram.bin" 258112 "34120000")    # 0x3F040 written, 0x3F042 not

# first.gba plots a red pixel at the address in its literal pool at 0x08000368 (0x060096F0).
# The recompiler folds that literal into a constant; with the ROM patch the function must run
# in the interpreter and plot at 0x06000000 instead.
set(GAME "${FIRST_GAME}")
set(ROM "${FIRST_ROM}")
run_game(plain --frames 30 --no-save --dump "vram:${WORK}/vram-plain.bin")
expect_bytes("${WORK}/vram-plain.bin" 38640 "1f00")   # 0x96F0
expect_bytes("${WORK}/vram-plain.bin" 0 "0000")
run_game(patched --frames 30 --no-save --dump "vram:${WORK}/vram-patched.bin" --cheat "gsv1raw:600001B4 00000000")
expect_bytes("${WORK}/vram-patched.bin" 0 "1f00")
expect_bytes("${WORK}/vram-patched.bin" 38640 "0000")

# A bad code stops with a message naming it.
execute_process(COMMAND "${GAME}" "${ROM}" --headless --frames 5 --no-save --cheat "DEADFACE 00001234"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "DEADFACE")
  message(FATAL_ERROR "a DEADFACE code should be refused:\n${out}\n${err}")
endif()
