# libanygba: the HLE runtime that recompiled game code links against.
add_library(anygba_runtime STATIC
  src/runtime.cpp
  src/system.cpp
  src/bus.cpp
  src/io_regs.cpp
  src/scheduler.cpp
  src/irq.cpp
  src/dma.cpp
  src/timers.cpp
  src/cpu_state.cpp
  src/bios/swi_table.cpp
  src/bios/boot_state.cpp
  src/bios/math.cpp
  src/bios/memcpy.cpp
  src/bios/decompress.cpp
  src/bios/affine.cpp
  src/ppu/ppu.cpp
  src/ppu/bg_text.cpp
  src/ppu/bg_affine.cpp
  src/ppu/bg_bitmap.cpp
  src/ppu/sprites.cpp
  src/ppu/compose.cpp
  src/apu/apu.cpp
  src/apu/psg.cpp
  src/apu/fifo.cpp
  src/apu/mixer.cpp
  src/save/detect.cpp
  src/save/sram.cpp
  src/save/flash.cpp
  src/save/eeprom.cpp
  src/save/save_file.cpp
  src/input_map.cpp
  src/state/state.cpp
  src/rom_patch.cpp
  src/rtc.cpp
  src/cheats/codes.cpp
  src/cheats/engine.cpp
  src/cheats/cheat_list.cpp
  src/player/osd.cpp
  src/player/speed.cpp
  src/player/script.cpp
  src/player/menu.cpp
  src/video/parallel.cpp
  src/video/color.cpp
  src/video/scalers.cpp
  src/video/hqx.cpp
  src/video/xbr.cpp
  src/video/pipeline.cpp
  src/video/presenter.cpp
  src/video/settings.cpp
  src/player/touch.cpp
  src/player/touch_overlay.cpp
  src/player/lifecycle.cpp
  src/control/json.cpp
  src/control/png.cpp
  src/control/net.cpp
  src/control/server.cpp
  src/control/memory.cpp
  src/control/symbols.cpp
  src/control/controller.cpp
  src/journal/journal.cpp
  src/dispatch.cpp
  src/interp/arm_interp.cpp
  src/interp/thumb_interp.cpp)
add_library(anygba::runtime ALIAS anygba_runtime)

target_include_directories(anygba_runtime
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE src)
target_compile_definitions(anygba_runtime PRIVATE ANYGBA_VERSION="${PROJECT_VERSION}")
# toml++ is header-only and compiled into the library, so installed games do not need it.
target_include_directories(anygba_runtime SYSTEM PRIVATE "$<BUILD_INTERFACE:${tomlplusplus_SOURCE_DIR}>")
target_link_libraries(anygba_runtime PUBLIC anyport::platform PRIVATE anygba_decode anygba::common)
if(WIN32)
  target_link_libraries(anygba_runtime PUBLIC ws2_32)  # the agent control server
endif()
set_target_properties(anygba_runtime PROPERTIES OUTPUT_NAME anygba)
anygba_configure_target(anygba_runtime)

# main() for generated games: loads the ROM and runs anygba_game_info from dispatch.c.
add_library(anygba_game_main STATIC src/main.cpp)
add_library(anygba::game_main ALIAS anygba_game_main)
target_link_libraries(anygba_game_main PUBLIC anygba_runtime PRIVATE anygba::common)
target_include_directories(anygba_game_main PRIVATE src)
anygba_configure_target(anygba_game_main)

if(ANDROID)
  # stdout and stderr go nowhere on Android; forward them to logcat (tag "anygba").
  target_sources(anygba_game_main PRIVATE src/android/log_forward.cpp)
  target_link_libraries(anygba_game_main PRIVATE log)
endif()
