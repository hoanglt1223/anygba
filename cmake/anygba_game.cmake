# anygba_add_game(<target> ROM <rom.gba> [CONFIG <game.toml>] [SHARDS <n>] [MISS_LOGS <log>...])
#
# Recompiles ROM with the in-tree anygba-recomp as part of the build and links the result
# into the executable <target>. Used by the corpus tests; installed projects use the
# CMakeLists.txt that anygba-recomp writes instead.
function(anygba_add_game target)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "ROM;CONFIG;SHARDS" "MISS_LOGS")
  if(NOT ARG_ROM)
    message(FATAL_ERROR "anygba_add_game(${target}): ROM is required")
  endif()
  if(NOT ARG_SHARDS)
    set(ARG_SHARDS 4)
  endif()
  set(out "${CMAKE_CURRENT_BINARY_DIR}/${target}-gen")
  set(sources "${out}/dispatch.c")
  math(EXPR last "${ARG_SHARDS} - 1")
  foreach(i RANGE ${last})
    string(LENGTH "${i}" len)
    if(len EQUAL 1)
      set(i "00${i}")
    elseif(len EQUAL 2)
      set(i "0${i}")
    endif()
    list(APPEND sources "${out}/funcs_${i}.c")
  endforeach()

  set(args "${ARG_ROM}" --output "${out}" --shards ${ARG_SHARDS})
  set(deps anygba-recomp "${ARG_ROM}")
  if(ARG_CONFIG)
    list(APPEND args --config "${ARG_CONFIG}")
    list(APPEND deps "${ARG_CONFIG}")
  endif()
  foreach(log IN LISTS ARG_MISS_LOGS)
    list(APPEND args --miss-log "${log}")
    list(APPEND deps "${log}")
  endforeach()

  add_custom_command(
    OUTPUT ${sources} "${out}/game.h"
    COMMAND anygba-recomp ${args}
    DEPENDS ${deps}
    COMMENT "Recompiling ${ARG_ROM}"
    VERBATIM)
  add_executable(${target} ${sources})
  target_link_libraries(${target} PRIVATE anygba::game_main)
endfunction()
