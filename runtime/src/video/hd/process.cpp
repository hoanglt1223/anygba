#include "video/hd/process.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
extern char** environ;
#endif

namespace anygba::video::hd {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quotes one argument for CommandLineToArgvW-style parsing.
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

}  // namespace

std::optional<std::string> run_process(const std::vector<std::string>& args, const std::atomic<bool>* cancel,
                                       const std::function<void()>& tick) {
    if (args.empty()) return "no program to run";
    std::wstring line;
    for (const auto& a : args) {
        if (!line.empty()) line += L' ';
        line += quote(widen(a));
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return "cannot run " + args[0] + " (error " + std::to_string(GetLastError()) + ")";
    }
    while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT) {
        if (cancel && *cancel) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return kCancelled;
        }
        if (tick) tick();
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) return args[0] + " failed with exit code " + std::to_string(code);
    return std::nullopt;
}

#else

std::optional<std::string> run_process(const std::vector<std::string>& args, const std::atomic<bool>* cancel,
                                       const std::function<void()>& tick) {
    if (args.empty()) return "no program to run";
#if defined(__ANDROID__) && __ANDROID_API__ < 28
    // posix_spawn needs API 28; the Android runtime has no use for helper programs.
    (void)cancel;
    (void)tick;
    return "running " + args[0] + " is not supported on this Android version";
#else
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return "cannot run " + args[0];
    int status = 0;
    for (int ticks = 0;; ++ticks) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) return "cannot wait for " + args[0];
        if (cancel && *cancel) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return kCancelled;
        }
        if (tick && ticks % 10 == 9) tick();
        const timespec pause{0, 20'000'000};
        nanosleep(&pause, nullptr);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return args[0] + " failed";
    return std::nullopt;
#endif
}

#endif

}  // namespace anygba::video::hd
