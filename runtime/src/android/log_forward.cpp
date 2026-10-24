// Android discards stdout and stderr. The player reports everything there (frame hashes,
// warnings, the menu trace), so a thread copies both into logcat under the tag "anygba".
#include <android/log.h>

#include <pthread.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

void* forward(void* arg) {
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(arg));
    char buf[1024];
    std::size_t used = 0;
    for (;;) {
        const ssize_t n = read(fd, buf + used, sizeof buf - 1 - used);
        if (n <= 0) break;
        used += static_cast<std::size_t>(n);
        std::size_t start = 0;
        for (std::size_t i = 0; i < used; ++i) {
            if (buf[i] == '\n') {
                buf[i] = '\0';
                __android_log_write(ANDROID_LOG_INFO, "anygba", buf + start);
                start = i + 1;
            }
        }
        if (start == 0 && used == sizeof buf - 1) {  // a line longer than the buffer
            buf[used] = '\0';
            __android_log_write(ANDROID_LOG_INFO, "anygba", buf);
            used = 0;
        } else {
            std::memmove(buf, buf + start, used - start);
            used -= start;
        }
    }
    return nullptr;
}

struct LogForward {
    LogForward() {
        int fds[2];
        if (pipe(fds) != 0) return;
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        std::setvbuf(stderr, nullptr, _IOLBF, 0);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        pthread_t thread;
        if (pthread_create(&thread, nullptr, forward, reinterpret_cast<void*>(static_cast<std::intptr_t>(fds[0]))) == 0) {
            pthread_detach(thread);
        }
    }
};

const LogForward log_forward;

}  // namespace
