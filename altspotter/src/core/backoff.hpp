#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>

namespace altspotter {

// Exponential backoff with full jitter: delay ∈ [min, min(cap, base * 2^attempt)].
class Backoff {
public:
    Backoff(std::chrono::milliseconds min_delay, std::chrono::milliseconds max_delay)
        : min_(min_delay), max_(max_delay), rng_(std::random_device{}()) {}

    std::chrono::milliseconds next() {
        const auto ceiling = current_ceiling();
        if (attempt_ < 63) ++attempt_;
        std::uniform_int_distribution<int64_t> dist(min_.count(), ceiling.count());
        return std::chrono::milliseconds(dist(rng_));
    }

    void reset() noexcept { attempt_ = 0; }
    [[nodiscard]] uint32_t attempt() const noexcept { return attempt_; }

private:
    [[nodiscard]] std::chrono::milliseconds current_ceiling() const {
        const uint32_t shift = std::min<uint32_t>(attempt_, 20);
        const int64_t scaled = min_.count() << shift;
        return std::chrono::milliseconds(
            std::clamp<int64_t>(scaled, min_.count(), max_.count()));
    }

    std::chrono::milliseconds min_;
    std::chrono::milliseconds max_;
    uint32_t attempt_ = 0;
    std::mt19937_64 rng_;
};

} // namespace altspotter
