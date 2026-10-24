// Downloads of ONNX Runtime and models on first AI use: progress, cancelling, and failures leave
// no partial files. Nothing here reaches the internet: sources are file:// URLs, and a local
// socket that accepts connections but never answers stands in for a slow server.
#include "support/temp_dir.h"
#include "anygba/sha256.h"
#include "video/hd/ai.h"
#include "video/hd/process.h"
#include "video/hd/session.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace hd = anygba::video::hd;
namespace fs = std::filesystem;

namespace {

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

// Sets an environment variable for one test and clears it afterwards.
struct ScopedEnv {
    const char* name;
    ScopedEnv(const char* n, const std::string& value) : name(n) { set_env(n, value); }
    ~ScopedEnv() { set_env(name, ""); }
};

std::string file_url(const fs::path& p) {
    std::string s = fs::absolute(p).generic_string();
    return s.front() == '/' ? "file://" + s : "file:///" + s;
}

std::vector<fs::path> files_ending(const fs::path& dir, const std::string& suffix) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            out.push_back(it->path());
        }
    }
    return out;
}

// A TCP port on 127.0.0.1 that completes connections (the listen backlog) but never answers, so a
// download from it runs until it is cancelled.
class SilentServer {
public:
    SilentServer() {
#ifdef _WIN32
        WSADATA wsa;
        REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        REQUIRE(listen(sock_, 4) == 0);
#ifdef _WIN32
        int len = sizeof addr;
#else
        socklen_t len = sizeof addr;
#endif
        REQUIRE(getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
    }
    ~SilentServer() {
#ifdef _WIN32
        closesocket(sock_);
        WSACleanup();
#else
        close(sock_);
#endif
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
#ifdef _WIN32
    SOCKET sock_;
#else
    int sock_;
#endif
    int port_ = 0;
};

// Collects session messages until one contains `text` or `seconds` pass.
bool wait_for_message(hd::Session& s, const std::string& text, std::vector<std::string>& seen, double seconds = 15) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        for (auto& m : s.take_messages()) seen.push_back(std::move(m));
        for (const auto& m : seen) {
            if (m.find(text) != std::string::npos) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}  // namespace

TEST_CASE("ai download: file source, hash check, no partial file on failure") {
    anygba::test::TempDir dir("anygba-ai-download");
    const fs::path source = dir.path / "src" / "archive.zip";
    fs::create_directories(source.parent_path());
    const std::vector<std::uint8_t> bytes(300000, 0x5A);
    std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const std::string sha = anygba::sha256_hex(bytes);

    std::vector<std::string> progress;
    hd::ai::Fetch fetch;
    fetch.progress = [&](const std::string& line) { progress.push_back(line); };
    const fs::path target = dir.path / "cache" / "archive.zip";
    CHECK_FALSE(hd::ai::download(file_url(source), sha, target, "test archive", fetch));
    CHECK(fs::file_size(target) == bytes.size());
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.front().starts_with("test archive: "));

    // A wrong hash is rejected and leaves nothing behind.
    const fs::path bad = dir.path / "cache" / "bad.zip";
    const auto error = hd::ai::download(file_url(source), std::string(64, '0'), bad, "bad", {});
    REQUIRE(error);
    CHECK(error->find("SHA-256") != std::string::npos);
    CHECK_FALSE(fs::exists(bad));
    CHECK(files_ending(dir.path / "cache", ".part").empty());

    // ANYGBA_AI_MIRROR replaces everything up to the file name.
    {
        ScopedEnv mirror("ANYGBA_AI_MIRROR", file_url(source.parent_path()));
        const fs::path mirrored = dir.path / "cache2" / "archive.zip";
        CHECK_FALSE(hd::ai::download("https://example.invalid/x/archive.zip", sha, mirrored, "m", {}));
        CHECK(fs::exists(mirrored));
    }

    // Cancelled before it starts.
    std::atomic<bool> cancel{true};
    hd::ai::Fetch stopped;
    stopped.cancel = &cancel;
    const auto c = hd::ai::download(file_url(source), sha, dir.path / "cache3" / "archive.zip", "c", stopped);
    REQUIRE(c);
    CHECK(*c == hd::kCancelled);
}

TEST_CASE("ai download: cancelling a stalled download is prompt and leaves no partial file") {
    anygba::test::TempDir dir("anygba-ai-cancel");
    SilentServer server;
    std::atomic<bool> cancel{false};
    hd::ai::Fetch fetch;
    fetch.cancel = &cancel;
    std::optional<std::string> result;
    std::thread t([&] { result = hd::ai::download(server.url() + "/runtime.nupkg", std::string(64, '0'), dir.path / "runtime.nupkg", "rt", fetch); });
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto start = std::chrono::steady_clock::now();
    cancel = true;
    t.join();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    REQUIRE(result);
    CHECK(*result == hd::kCancelled);
    CHECK(files_ending(dir.path, ".part").empty());
    CHECK_FALSE(fs::exists(dir.path / "runtime.nupkg"));
}

// The player's path: turning AI on starts the download on the session's worker thread with
// progress messages; turning it off again returns at once and the picture stays without AI.
TEST_CASE("ai download: session shows progress, cancels on AI off, and reports failures") {
    anygba::test::TempDir dir("anygba-ai-session");
    ScopedEnv cache("ANYGBA_AI_CACHE", (dir.path / "cache").string());
    ScopedEnv runtime("ANYGBA_ONNXRUNTIME", (dir.path / "no-runtime" / "missing.dll").string());

    hd::SessionSetup setup;
    setup.game_code = "TEST";
    anygba::video::Options on;
    on.ai = anygba::video::Ai::Frame;

    SUBCASE("cancel") {
        SilentServer server;
        ScopedEnv mirror("ANYGBA_AI_MIRROR", server.url());
        hd::Session s(setup);
        s.set_options(on);
        std::vector<std::string> seen;
#ifdef _WIN32
        const std::string first = "AI download: ONNX Runtime";
#else
        const std::string first = "AI download: ";  // ONNX Runtime is not downloaded off Windows
        if (!wait_for_message(s, "AI download: ", seen, 3)) {
            MESSAGE("skipped: no download on this platform without ONNX Runtime");
            return;
        }
#endif
        REQUIRE(wait_for_message(s, first, seen));
        CHECK(seen.back().starts_with(hd::Session::kAiDownloadPrefix));
        const auto start = std::chrono::steady_clock::now();
        s.set_options({});
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        CHECK(wait_for_message(s, "AI upscaling cancelled", seen, 2));
        CHECK(files_ending(dir.path, ".part").empty());
    }

    SUBCASE("failure") {
        fs::create_directories(dir.path / "empty");
        ScopedEnv mirror("ANYGBA_AI_MIRROR", file_url(dir.path / "empty"));
        hd::Session s(setup);
        s.set_options(on);
        std::vector<std::string> seen;
        CHECK(wait_for_message(s, "AI upscaling unavailable", seen));
        CHECK(files_ending(dir.path, ".part").empty());
        CHECK(s.active());  // still drawn by the HD path, without AI
    }
}
