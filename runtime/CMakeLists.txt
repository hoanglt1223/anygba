# libanygba: the HLE runtime that recompiled game code links against.
add_library(anygba_runtime STATIC
  src/runtime.cpp)
add_library(anygba::runtime ALIAS anygba_runtime)

target_include_directories(anygba_runtime PUBLIC include)
target_compile_definitions(anygba_runtime PRIVATE ANYGBA_VERSION="${PROJECT_VERSION}")
target_link_libraries(anygba_runtime PUBLIC anyport::platform)
set_target_properties(anygba_runtime PROPERTIES OUTPUT_NAME anygba)
anygba_configure_target(anygba_runtime)
