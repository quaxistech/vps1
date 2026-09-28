#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace altspotter {

// Thread-safe token bucket. acquire() blocks until a token is available
// or the deadline passes; returns false on deadline/stop.
class TokenBucket {
public:
    TokenBucket(double tokens_per_second, double burst)
        : rate_(tokens_per_second), burst_(burst), tokens_(burst),
          last_(std::chrono::steady_clock::now()) {}

    bool try_acquire() {
        std::lock_guard lock(mu_);
        refill();
        if (tokens_ >= 1.0) {
            tokens_ -= 1.0;
            return true;
        }
        return false;
    }

    // Blocking acquire with polling; safe for shutdown via stop flag check by caller.
    template <typename StopPredicate>
    bool acquire(StopPredicate should_stop,
                 std::chrono::milliseconds poll = std::chrono::milliseconds(20)) {
        while (!should_stop()) {
            if (try_acquire()) return true;
            std::this_thread::sleep_for(poll);
        }
        return false;
    }

private:
    void refill() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        tokens_ = std::min(burst_, tokens_ + elapsed * rate_);
    }

    std::mutex mu_;
    double rate_;
    double burst_;
    double tokens_;
    std::chrono::steady_clock::time_point last_;
};

} // namespace altspotter
