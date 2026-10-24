# Importing and exporting .sav files (VisualBoyAdvance / mGBA raw chip contents) with the SRAM
# save test: --export-save, --import-save with a trailing footer, a refused size, the backup
# of the save it replaces, and the menu's Options page in a headless run (next to the ROM).
#   cmake -DGAME=<exe> -DROM=<sram.gba> -DWORK=<dir> -P player-saveio.cmake
include("${CMAKE_CURRENT_LIST_DIR}/player-common.cmake")

# A copy of the ROM: the menu exports next to the ROM, which must not be the corpus folder.
file(COPY_FILE "${ROM}" "${WORK}/sram.gba")
set(ROM "${WORK}/sram.gba")

run_game(out --exit-on-idle --frames 600 --save-dir "${WORK}/a" --export-save "${WORK}/export.sav")
file(SIZE "${WORK}/export.sav" bytes)
if(NOT bytes EQUAL 32768)
  message(FATAL_ERROR "export.sav is ${bytes} bytes, expected 32768")
endif()
file(GLOB saved "${WORK}/a/*.sav")
file(SHA256 "${saved}" saved_hash)
file(SHA256 "${WORK}/export.sav" export_hash)
if(NOT saved_hash STREQUAL export_hash)
  message(FATAL_ERROR "the export differs from the save file ${saved}")
endif()

# A file with a 16-byte footer (VisualBoyAdvance-M's clock bytes) is trimmed and loaded.
file(WRITE "${WORK}/footer.bin" "VBA-M RTC footer")  # 16 bytes
execute_process(COMMAND "${CMAKE_COMMAND}" -E cat "${WORK}/export.sav" "${WORK}/footer.bin"
  OUTPUT_FILE "${WORK}/vba.sav" RESULT_VARIABLE rc)
file(SIZE "${WORK}/vba.sav" bytes)
if(NOT rc EQUAL 0 OR NOT bytes EQUAL 32784)
  message(FATAL_ERROR "could not make vba.sav (${bytes} bytes)")
endif()
run_game(out --frames 5 --save-dir "${WORK}/b" --import-save "${WORK}/vba.sav")
expect_output("${out}" "osd: Imported vba.sav")
file(GLOB imported "${WORK}/b/*.sav")
file(SHA256 "${imported}" imported_hash)
if(NOT imported_hash STREQUAL export_hash)
  message(FATAL_ERROR "the imported save ${imported} differs from the exported one")
endif()
# Importing over an existing save keeps that save first.
run_game(out --frames 5 --save-dir "${WORK}/b" --import-save "${WORK}/vba.sav")
file(GLOB kept "${WORK}/b/*.sav.before-import-*")
if(NOT kept)
  message(FATAL_ERROR "no .before-import copy of the replaced save")
endif()

# A size that fits no SRAM is refused, and the program says why.
file(WRITE "${WORK}/bad.sav" "not a save file")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "ANYGBA_DATA_DIR=${WORK}/data" "${GAME}" "${ROM}" --headless --no-miss-log
    --frames 5 --save-dir "${WORK}/b" --import-save "${WORK}/bad.sav"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "does not fit the sram save")
  message(FATAL_ERROR "bad.sav was not refused (exit ${rc}):\n${out}\n${err}")
endif()

# The menu: Settings > Export save writes sram.sav next to the ROM; Import save loads it back
# and restarts the game. L (the A key) from the first tab goes to the last one, Settings; Up
# from its first row wraps to Import, then Export.
file(REMOVE "${WORK}/sram.sav")
file(WRITE "${WORK}/menu.txt" "
10 key tap Escape
14 key tap A
16 key tap Up
18 key tap Up
20 key tap Z
22 key tap Down
24 key tap Z
")
run_game(out --frames 60 --save-dir "${WORK}/a" --script "${WORK}/menu.txt")
expect_output("${out}" "menu\\| == Settings ==")
expect_output("${out}" "osd: Exported sram.sav")
expect_output("${out}" "osd: Imported sram.sav; game restarted")
file(SHA256 "${WORK}/sram.sav" menu_hash)
if(NOT menu_hash STREQUAL export_hash)
  message(FATAL_ERROR "the menu export differs from --export-save")
endif()
