# Host platform layer (window, audio, input, timing, log) on SDL3.
# Deliberately free of GBA specifics so other anyport projects can reuse it.
add_library(anyport_platform STATIC
  src/context.cpp
  src/window.cpp
  src/audio.cpp
  src/input.cpp
  src/timing.cpp
  src/log.cpp)
add_library(anyport::platform ALIAS anyport_platform)

target_include_directories(anyport_platform PUBLIC include)
target_link_libraries(anyport_platform PUBLIC SDL3::SDL3)
anygba_configure_target(anyport_platform)
