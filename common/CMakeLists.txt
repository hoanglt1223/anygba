# Small utilities shared by the recompiler and the runtime.
add_library(anygba_common STATIC src/sha256.cpp src/embedded_rom.cpp)
add_library(anygba::common ALIAS anygba_common)
target_include_directories(anygba_common PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
anygba_configure_target(anygba_common)
