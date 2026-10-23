add_library(anygba_recomp_lib STATIC
  src/cli.cpp)
target_include_directories(anygba_recomp_lib PUBLIC src)
target_compile_definitions(anygba_recomp_lib PRIVATE ANYGBA_VERSION="${PROJECT_VERSION}")
anygba_configure_target(anygba_recomp_lib)

add_executable(anygba-recomp src/main.cpp)
target_link_libraries(anygba-recomp PRIVATE anygba_recomp_lib)
anygba_configure_target(anygba-recomp)
