# ARMv4T decoder and disassembler, shared by the recompiler and the runtime's fallback interpreter.
add_library(anygba_decode STATIC
  decode/arm.cpp
  decode/thumb.cpp
  decode/disasm.cpp)
target_include_directories(anygba_decode PUBLIC .)
anygba_configure_target(anygba_decode)

add_library(anygba_recomp_lib STATIC
  src/cli.cpp
  src/pipeline.cpp
  analysis/image.cpp
  analysis/cfg.cpp
  analysis/functions.cpp
  analysis/jumptables.cpp
  config/game_config.cpp
  io/miss_log.cpp)
target_include_directories(anygba_recomp_lib PUBLIC src .)
target_compile_definitions(anygba_recomp_lib PRIVATE ANYGBA_VERSION="${PROJECT_VERSION}")
target_link_libraries(anygba_recomp_lib PUBLIC anygba_decode tomlplusplus::tomlplusplus)
anygba_configure_target(anygba_recomp_lib)

add_executable(anygba-recomp src/main.cpp)
target_link_libraries(anygba-recomp PRIVATE anygba_recomp_lib)
anygba_configure_target(anygba-recomp)
