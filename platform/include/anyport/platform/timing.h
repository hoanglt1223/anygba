#pragma once

#include <cstdint>

namespace anyport::platform {

std::uint64_t now_ns();
void sleep_ns(std::uint64_t duration);

// Paces a loop to a fixed rate. If the caller falls more than one period behind, the
// schedule restarts from now instead of rushing to catch up.
class FramePacer {
public:
    explicit FramePacer(double frames_per_second);

    void wait();
    std::uint64_t period_ns() const noexcept { return period_ns_; }

private:
    std::uint64_t period_ns_;
    std::uint64_t next_ns_ = 0;
};

}  // namespace anyport::platform
