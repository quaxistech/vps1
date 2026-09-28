#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

namespace altmacro::core {

// Token bucket used to hold every provider inside its budget.
//
// Two of our providers publish a limit and the rest do not, so the bucket is
// also the place where an *undocumented* limit is respected conservatively:
// the refill rate is whatever the registry says, and the registry records
// whether that number was read or measured.
class TokenBucket {
public:
    TokenBucket(double tokens_per_second, double burst);

    // Blocks until a token is available, the deadline passes, or `should_stop`
    // returns true. Returns false if it gave up rather than acquired.
    bool acquire(const std::function<bool()>& should_stop,
                 std::chrono::milliseconds max_wait = std::chrono::minutes(10));

    // Back off after a 429/503: the server has told us our rate is wrong, so
    // the bucket is drained and refill is suspended for `penalty`.
    void penalise(std::chrono::milliseconds penalty);

private:
    void refill_locked(std::chrono::steady_clock::time_point now);

    std::mutex mu_;
    double rate_;
    double burst_;
    double tokens_;
    std::chrono::steady_clock::time_point last_;
    std::chrono::steady_clock::time_point suspended_until_;
};

// Bounded retry with exponential backoff and full jitter, honouring shutdown.
//
// `attempt` returns the HTTP status; the caller decides which statuses are
// retryable. 429 and 503 additionally penalise the bucket, because retrying at
// the same cadence after being told to slow down is how an IP gets blocked —
// which is exactly what Dukascopy does to a burst.
struct RetryPolicy {
    int32_t max_attempts = 5;
    int32_t base_delay_ms = 1000;
    int32_t max_delay_ms = 120000;
};

} // namespace altmacro::core
