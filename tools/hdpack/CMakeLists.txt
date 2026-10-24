# `anygba hdpack`: HD pack creator (dedupe, preview, import, AI upscaling), built into the
# anygba driver. It uses the runtime's HD code (keys, packs, PNG files, the AI upscaler).
add_library(anygba_hdpack_lib STATIC hdpack.cpp)
target_include_directories(anygba_hdpack_lib PUBLIC . PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_link_libraries(anygba_hdpack_lib PRIVATE anygba_runtime anygba::common)
anygba_configure_target(anygba_hdpack_lib)
