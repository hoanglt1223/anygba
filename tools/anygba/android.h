#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// `anygba build --target android|android-package` and `anygba android-launcher`: NDK
// cross-builds of the generated C against the runtime in <prefix>/android/<abi>, and APKs
// assembled straight from the SDK build tools (aapt2, javac, d8, zipalign, apksigner)
// without Gradle. Layout and decisions: docs/user/ANDROID.md, docs/dev/ANDROID.md.
namespace anygba::driver::android {

struct Options {
    std::vector<std::string> abis = {"arm64-v8a", "x86_64"};
    std::optional<std::filesystem::path> sdk;  // default: ANDROID_HOME, ANDROID_SDK_ROOT, per-user default
    std::optional<std::filesystem::path> ndk;  // default: ANDROID_NDK_HOME, ANDROID_NDK_ROOT, newest <sdk>/ndk/*
    std::optional<std::string> package_id;     // default: io.anyport.anygba.g<first 12 hex digits of the ROM SHA-256>
    std::optional<std::filesystem::path> icon;  // PNG; default: the template icon
    std::optional<std::filesystem::path> keystore;  // default: the Android debug keystore (created if missing)
    std::string key_alias = "androiddebugkey";
    std::optional<std::string> keystore_pass_env;  // environment variable holding the keystore password
    std::optional<int> version_code;               // default: minutes since 2024-01-01, so rebuilds update
};

inline constexpr std::string_view kAbis[] = {"arm64-v8a", "armeabi-v7a", "x86_64", "x86"};

// Parses one Android option at args[i] (advancing i past its value). Returns nullopt when
// args[i] is not an Android option, else an error message ("" on success).
std::optional<std::string> parse_option(std::span<const std::string_view> args, std::size_t& i, Options& out);
std::string usage();

// Java package rules: two or more '.'-separated segments, each a letter followed by
// letters, digits or '_'.
bool valid_package_id(std::string_view id);
std::string default_package_id(std::string_view rom_sha256);
// "<title>-<sha8>", the file stem the player gives saves and save states. Must match
// game_file_stem() in runtime/src/main.cpp.
std::string save_stem(std::string_view title, std::string_view rom_sha256);
std::string xml_escape(std::string_view text);
// Replaces each @KEY@ in `text`; throws std::runtime_error naming a placeholder left over.
std::string fill_template(std::string text, const std::vector<std::pair<std::string, std::string>>& values);

struct Game {
    std::filesystem::path gen_dir;  // anygba-recomp output
    std::filesystem::path rom;
    std::string title;
    std::string name;  // app label and file name
    std::string rom_sha256;
    std::vector<std::filesystem::path> cheat_databases;
};

struct Paths {
    std::filesystem::path prefix;  // anygba install prefix (share/anygba/android, android/<abi>)
    std::filesystem::path work;
    std::filesystem::path output_dir;
    std::string cmake = "cmake";
    std::optional<std::string> generator;  // default: Ninja
    std::string build_type = "Release";
};

// Writes <output>/<name>.apk: one game, ROM included, signed. Returns the path.
std::filesystem::path build_apk(const Game& game, const Paths& paths, const Options& options);
// Writes <output>/<name>.agp, a game package for the launcher app. Returns the path.
std::filesystem::path build_package(const Game& game, const Paths& paths, const Options& options);
// Writes <output>/AnyGBA-Launcher.apk. Returns the path.
std::filesystem::path build_launcher(const Paths& paths, const Options& options);

}  // namespace anygba::driver::android
