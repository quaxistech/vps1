#include "altmacro/core/rate_limiter.hpp"

#include <algorithm>
#include <thread>

namespace altmacro::core {

TokenBucket::TokenBucket(double tokens_per_second, double burst)
    : rate_(std::max(0.001, tokens_per_second)),
      burst_(std::max(1.0, burst)),
      tokens_(std::max(1.0, burst)),
      last_(std::chrono::steady_clock::now()),
      suspended_until_(std::chrono::steady_clock::now()) {}

void TokenBucket::refill_locked(std::chrono::steady_clock::time_point now) {
    // While a penalty is in force the bucket does not refill at all. Refilling
    // through a penalty would let a burst resume the instant it expires, which
    // is the behaviour that got us throttled in the first place.
    if (now < suspended_until_) {
        last_ = now;
        return;
    }
    const auto start = std::max(last_, suspended_until_);
    if (now <= start) return;
    const double seconds = std::chrono::duration<double>(now - start).count();
    tokens_ = std::min(burst_, tokens_ + seconds * rate_);
    last_ = now;
}

bool TokenBucket::acquire(const std::function<bool()>& should_stop,
                          std::chrono::milliseconds max_wait) {
    const auto deadline = std::chrono::steady_clock::now() + max_wait;
    for (;;) {
        if (should_stop && should_stop()) return false;
        std::chrono::milliseconds sleep_for{0};
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto now = std::chrono::steady_clock::now();
            refill_locked(now);
            if (now >= suspended_until_ && tokens_ >= 1.0) {
                tokens_ -= 1.0;
                return true;
            }
            if (now < suspended_until_) {
                sleep_for = std::chrono::duration_cast<std::chrono::milliseconds>(
                    suspended_until_ - now);
            } else {
                const double deficit = 1.0 - tokens_;
                sleep_for = std::chrono::milliseconds(
                    static_cast<int64_t>((deficit / rate_) * 1000.0) + 5);
            }
        }
        if (std::chrono::steady_clock::now() + sleep_for > deadline) return false;
        // Sleep in slices so shutdown is honoured promptly even when the wait
        // is minutes long, which it is after a provider penalty.
        const auto slice = std::min(sleep_for, std::chrono::milliseconds(250));
        std::this_thread::sleep_for(slice);
    }
}

void TokenBucket::penalise(std::chrono::milliseconds penalty) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = std::chrono::steady_clock::now();
    tokens_ = 0.0;
    suspended_until_ = std::max(suspended_until_, now + penalty);
    last_ = now;
}

} // namespace altmacro::core
