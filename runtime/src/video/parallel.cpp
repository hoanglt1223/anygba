#include "video/image.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace anygba::video {

namespace {

// Workers sleep between frames; each job is split into equal parts that the caller and the
// workers claim through an atomic counter.
class Pool {
public:
    Pool() {
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned workers = std::min(hw, 16u) - 1;
        for (unsigned i = 0; i < workers; ++i) threads_.emplace_back([this] { work(); });
    }

    ~Pool() {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_) t.join();
    }

    int threads() const { return static_cast<int>(threads_.size()) + 1; }

    void run(int count, const std::function<void(int, int)>& fn) {
        std::lock_guard serial(run_mutex_);
        const int parts = std::min(count, threads() * 2);
        {
            std::lock_guard lock(mutex_);
            fn_ = &fn;
            count_ = count;
            parts_ = parts;
            next_ = 0;
            done_ = 0;
            error_ = nullptr;
            ++generation_;
        }
        wake_.notify_all();
        claim();
        std::unique_lock lock(mutex_);
        finished_.wait(lock, [&] { return done_ == parts_; });
        fn_ = nullptr;
        if (error_) std::rethrow_exception(error_);
    }

private:
    void work() {
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return quit_ || generation_ != seen; });
                if (quit_) return;
                seen = generation_;
            }
            claim();
        }
    }

    void claim() {
        for (;;) {
            const int part = next_.fetch_add(1);
            if (part >= parts_) return;
            const int begin = static_cast<int>(static_cast<long long>(count_) * part / parts_);
            const int end = static_cast<int>(static_cast<long long>(count_) * (part + 1) / parts_);
            try {
                if (begin < end) (*fn_)(begin, end);
            } catch (...) {
                std::lock_guard lock(mutex_);
                if (!error_) error_ = std::current_exception();
            }
            std::lock_guard lock(mutex_);
            if (++done_ == parts_) finished_.notify_one();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex run_mutex_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable finished_;
    const std::function<void(int, int)>* fn_ = nullptr;
    int count_ = 0;
    int parts_ = 0;
    std::atomic<int> next_{0};
    int done_ = 0;
    std::exception_ptr error_;
    std::uint64_t generation_ = 0;
    bool quit_ = false;
};

}  // namespace

void parallel_for(int count, const std::function<void(int, int)>& fn) {
    if (count <= 0) return;
    if (count < 32) {
        fn(0, count);
        return;
    }
    static Pool pool;
    pool.run(count, fn);
}

}  // namespace anygba::video
