#include <anygba/runtime.h>

#include <doctest.h>

#include <string_view>

TEST_CASE("runtime reports a semantic version") {
    const std::string_view version = anygba_runtime_version();
    CHECK(version.size() >= 5);
    CHECK(version.find('.') != std::string_view::npos);
}
