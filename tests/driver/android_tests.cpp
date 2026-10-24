#include "android.h"
#include "build.h"
#include "zip.h"

#include "anygba/crc32.h"

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace anygba::driver;
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

fs::path scratch(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "anygba-android-tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::vector<std::uint8_t> read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("android: package ids") {
    CHECK(android::valid_package_id("io.anyport.anygba.g0123abcd"));
    CHECK(android::valid_package_id("a.b"));
    CHECK_FALSE(android::valid_package_id("single"));
    CHECK_FALSE(android::valid_package_id("io.1game"));
    CHECK_FALSE(android::valid_package_id("io..game"));
    CHECK_FALSE(android::valid_package_id("io.game-x"));
    CHECK_FALSE(android::valid_package_id("io.game."));
    CHECK(android::default_package_id("0123456789abcdef0123") == "io.anyport.anygba.g0123456789ab");
    CHECK(android::valid_package_id(android::default_package_id("ffffffffffffffff")));
}

TEST_CASE("android: save stems match the player's file names") {
    CHECK(android::save_stem("TONC IRQ", "deadbeef00") == "TONC_IRQ-deadbeef");
    CHECK(android::save_stem("", "0123456789") == "game-01234567");
    CHECK(android::save_stem("a/b", "0123456789") == "a_b-01234567");
}

TEST_CASE("android: templates and XML escaping") {
    CHECK(android::xml_escape("Tom & \"Jerry\" <3 'x'") == "Tom &amp; &quot;Jerry&quot; &lt;3 &apos;x&apos;");
    CHECK(android::fill_template("<a p=\"@PACKAGE@\" i=\"@mipmap/x\" q=\"@PACKAGE@\"/>", {{"PACKAGE", "a.b"}}) ==
          "<a p=\"a.b\" i=\"@mipmap/x\" q=\"a.b\"/>");
    CHECK_THROWS_AS(android::fill_template("x=\"@LABEL@\"", {{"PACKAGE", "a.b"}}), std::runtime_error);
}

TEST_CASE("android: build options") {
    BuildOptions o;
    const std::vector<std::string_view> args = {"g.gba",      "--target",        "android", "--abi",     "x86_64,arm64-v8a",
                                                "--package-id", "com.example.game", "--icon",  "i.png",     "--version-code",
                                                "42",         "--keystore",      "k.jks",   "--keystore-pass-env", "PASS"};
    REQUIRE_FALSE(parse_build_args(args, o));
    CHECK(o.target == Target::Android);
    CHECK(o.android.abis == std::vector<std::string>{"x86_64", "arm64-v8a"});
    CHECK(o.android.package_id == "com.example.game");
    CHECK(o.android.version_code == 42);
    CHECK(o.android.keystore_pass_env == "PASS");

    const auto error = [](std::vector<std::string_view> a) {
        BuildOptions fresh;
        return parse_build_args(a, fresh).value_or("");
    };
    CHECK(error({"g.gba", "--target", "ios"}).starts_with("unknown target"));
    CHECK(error({"g.gba", "--abi", "mips"}).starts_with("unknown ABI"));
    CHECK_FALSE(error({"g.gba", "--package-id", "nope"}).empty());
    CHECK_FALSE(error({"g.gba", "--version-code", "0"}).empty());
    CHECK_FALSE(error({"g.gba", "--abi"}).empty());
    BuildOptions p;
    REQUIRE_FALSE(parse_build_args(std::vector<std::string_view>{"g.gba", "--target", "android-package"}, p));
    CHECK(p.target == Target::AndroidPackage);
}

TEST_CASE("zip: new archives store entries uncompressed with correct CRCs") {
    const fs::path dir = scratch("new");
    zip::write(dir / "a.zip", std::nullopt, {{"one.txt", bytes("hello")}, {"lib/x86_64/libmain.so", bytes("\x7f" "ELF")}});
    const auto listed = zip::list(dir / "a.zip");
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].name == "one.txt");
    CHECK(listed[0].method == 0);
    CHECK(listed[0].size == 5);
    CHECK(listed[0].crc32 == anygba::crc32(bytes("hello")));
    CHECK(listed[1].name == "lib/x86_64/libmain.so");
    // The data sits right after a 30-byte local header and the name.
    const auto raw = read(dir / "a.zip");
    CHECK(std::string(raw.begin() + 30 + 7, raw.begin() + 30 + 7 + 5) == "hello");
}

TEST_CASE("zip: merging keeps the base entries and replaces same-named ones") {
    const fs::path dir = scratch("merge");
    zip::write(dir / "base.zip", std::nullopt, {{"keep", bytes("k")}, {"replace", bytes("old")}});
    zip::write(dir / "out.zip", dir / "base.zip", {{"replace", bytes("newer")}, {"added", bytes("+")}});
    const auto listed = zip::list(dir / "out.zip");
    REQUIRE(listed.size() == 3);
    CHECK(listed[0].name == "keep");
    CHECK(listed[1].name == "replace");
    CHECK(listed[1].size == 5);
    CHECK(listed[2].name == "added");
    // Merging the merged archive again is stable.
    zip::write(dir / "again.zip", dir / "out.zip", {});
    CHECK(read(dir / "again.zip") == read(dir / "out.zip"));
}

TEST_CASE("zip: damaged archives are rejected") {
    const fs::path dir = scratch("bad");
    {
        std::ofstream(dir / "junk.zip", std::ios::binary) << "this is not a zip archive at all";
    }
    CHECK_THROWS_AS(zip::list(dir / "junk.zip"), std::runtime_error);
    zip::write(dir / "ok.zip", std::nullopt, {{"a", bytes("abc")}});
    auto raw = read(dir / "ok.zip");
    raw.resize(raw.size() - 30);  // cut into the central directory
    {
        std::ofstream out(dir / "cut.zip", std::ios::binary);
        out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    }
    CHECK_THROWS_AS(zip::list(dir / "cut.zip"), std::runtime_error);
}

TEST_CASE("zip: directory entries use forward slashes and sorted names") {
    const fs::path dir = scratch("tree");
    fs::create_directories(dir / "src" / "b");
    std::ofstream(dir / "src" / "b" / "y.class") << "y";
    std::ofstream(dir / "src" / "a.class") << "a";
    const auto entries = zip::directory_entries(dir / "src", "classes/");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].name == "classes/a.class");
    CHECK(entries[1].name == "classes/b/y.class");
}
