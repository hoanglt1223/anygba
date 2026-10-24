# Install layout for release packages: the recompiler, the runtime libraries with headers,
# SDL3 (static), and a CMake package so the CMakeLists.txt that anygba-recomp writes can
# `find_package(anygba)` and link anygba::game_main.
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

install(TARGETS anygba-recomp anygba anygba-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Exported as anygba::runtime, anygba::game_main, ... to match the in-tree aliases.
set_target_properties(anygba_runtime PROPERTIES EXPORT_NAME runtime)
set_target_properties(anygba_game_main PROPERTIES EXPORT_NAME game_main)
set_target_properties(anygba_decode PROPERTIES EXPORT_NAME decode)
set_target_properties(anygba_common PROPERTIES EXPORT_NAME common)
set_target_properties(anyport_platform PROPERTIES EXPORT_NAME platform)

install(TARGETS anygba_runtime anygba_game_main anygba_decode anygba_common anyport_platform
  EXPORT anygbaTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY
  "${PROJECT_SOURCE_DIR}/runtime/include/"
  "${PROJECT_SOURCE_DIR}/platform/include/"
  "${PROJECT_SOURCE_DIR}/common/include/"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT anygbaTargets
  NAMESPACE anygba::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/anygba)

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/anygbaConfig.cmake.in"
  "${PROJECT_BINARY_DIR}/anygbaConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/anygba)
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/anygbaConfigVersion.cmake"
  COMPATIBILITY SameMinorVersion)
install(FILES
  "${PROJECT_BINARY_DIR}/anygbaConfig.cmake"
  "${PROJECT_BINARY_DIR}/anygbaConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/anygba)

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/README.md" DESTINATION .)
# Curated cheat databases; `anygba build` copies the ones for the ROM next to the game.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/cheats/" DESTINATION ${CMAKE_INSTALL_DATADIR}/anygba/cheats
  FILES_MATCHING PATTERN "*.toml")
# Named memory for AI agents; `anygba build` copies the file for the ROM next to the game.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/compat/symbols/" DESTINATION ${CMAKE_INSTALL_DATADIR}/anygba/symbols
  FILES_MATCHING PATTERN "*.toml")
