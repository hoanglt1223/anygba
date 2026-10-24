# `anygba build game.gba`: recompile, compile against the installed runtime, package.
add_library(anygba_driver_lib STATIC build.cpp android.cpp zip.cpp)
target_compile_definitions(anygba_driver_lib PRIVATE ANYGBA_VERSION="${PROJECT_VERSION}")
target_include_directories(anygba_driver_lib PUBLIC .)
target_link_libraries(anygba_driver_lib PUBLIC anygba_recomp_lib anygba::common)
anygba_configure_target(anygba_driver_lib)

add_executable(anygba main.cpp)
target_link_libraries(anygba PRIVATE anygba_driver_lib)
anygba_configure_target(anygba)
