#include "process.h"

#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace anygba::mcp {

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quoting that CommandLineToArgvW and the MSVC runtime undo exactly.
std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(ch == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out += ch;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

}  // namespace

int spawn(const std::vector<std::string>& argv, const std::filesystem::path& log) {
    if (argv.empty()) throw std::invalid_argument("spawn: empty command");
    std::wstring line;
    for (const auto& a : argv) line += (line.empty() ? L"" : L" ") + quote(widen(a));
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(log.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE || in == INVALID_HANDLE_VALUE) {
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
        throw std::runtime_error("cannot open the game log " + log.string());
    }
    // Only these two handles are inherited: the game must not hold our own stdio pipes open.
    HANDLE inherit[2] = {in, out};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attr_buf(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = out;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                   nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(out);
    CloseHandle(in);
    if (!ok) throw std::runtime_error("cannot start " + argv[0] + " (error " + std::to_string(error) + ")");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(pi.dwProcessId);
}

bool process_alive(int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

#else

int spawn(const std::vector<std::string>& argv, const std::filesystem::path& log) {
    if (argv.empty()) throw std::invalid_argument("spawn: empty command");
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) throw std::runtime_error("cannot start " + argv[0]);
    // Reap it when it exits so it does not stay a zombie that still looks alive.
    std::signal(SIGCHLD, SIG_IGN);
    return static_cast<int>(pid);
}

bool process_alive(int pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM); }

#endif

}  // namespace anygba::mcp
