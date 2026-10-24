# anygba-mcp: MCP server (stdio) for AI agents; shipped next to anygba in release packages.
add_executable(anygba-mcp main.cpp tools.cpp process.cpp)
target_link_libraries(anygba-mcp PRIVATE anygba::runtime)
# The control protocol, JSON and journal code live in the runtime's private sources.
target_include_directories(anygba-mcp PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(anygba-mcp PRIVATE ANYGBA_VERSION="${PROJECT_VERSION}")
anygba_configure_target(anygba-mcp)
