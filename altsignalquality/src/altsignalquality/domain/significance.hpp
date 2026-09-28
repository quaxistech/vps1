#pragma once

// Statistical uncertainty for a filter's measured edge.
//
// "The accepted set earned +35 bps over 120 trades" is not a result until it
// comes with an interval. A filter certified on a point estimate is certified
// on noise: at this stack's dispersion, a hundred trades routinely produce a
// ±40 bps swing with no edge at all behind it.
//
// Two things make the naive interval wrong here, and both are handled:
//
//   * The returns are NOT independent. Signals overlap in time (a 48h holding
//     limit against signals minutes apart), share instruments, and share market
//     regimes, so an ordinary IID bootstrap understates the variance — often by
//     a lot. A MOVING BLOCK bootstrap resamples contiguous blocks instead,
//     preserving the dependence inside a block.
//   * The quantity of interest is not the mean of a fixed sample but the mean
//     of the ACCEPTED subset, whose size is itself random under resampling.
//     Each replicate therefore recomputes the subset it accepted.
//
// The block length follows the standard n^(1/3) rule for a block bootstrap of
// the mean, floored so a short sample still gets blocks rather than points.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace altsignalquality::domain {

struct Interval {
    double point = std::nan("");
    double lower = std::nan("");
    double upper = std::nan("");
    int64_t samples = 0;
    int64_t block_length = 0;
    [[nodiscard]] bool excludes_zero_below() const {
        return std::isfinite(lower) && lower > 0.0;
    }
};

[[nodiscard]] inline int64_t block_bootstrap_length(size_t n) {
    if (n < 8) return 1;
    const auto l = static_cast<int64_t>(std::cbrt(static_cast<double>(n)));
    return std::max<int64_t>(l, 2);
}

// Percentile interval for the mean of `values`, under a moving block bootstrap.
// `seed` is fixed by the caller so a training report is reproducible.
[[nodiscard]] inline Interval block_bootstrap_mean(const std::vector<double>& values,
                                                   double confidence = 0.95,
                                                   int replicates = 2000,
                                                   uint32_t seed = 20260823U) {
    Interval out;
    out.samples = static_cast<int64_t>(values.size());
    if (values.empty()) return out;
    out.point = std::accumulate(values.begin(), values.end(), 0.0) /
                static_cast<double>(values.size());
    if (values.size() < 8 || replicates < 100) return out;  // interval would be theatre

    const int64_t block = block_bootstrap_length(values.size());
    out.block_length = block;
    const size_t n = values.size();
    const size_t blocks_per_replicate = (n + static_cast<size_t>(block) - 1) /
                                        static_cast<size_t>(block);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> start(0, n - 1);

    std::vector<double> means;
    means.reserve(static_cast<size_t>(replicates));
    for (int r = 0; r < replicates; ++r) {
        double sum = 0;
        size_t taken = 0;
        for (size_t b = 0; b < blocks_per_replicate && taken < n; ++b) {
            // Circular blocks: every observation gets equal weight, including
            // the ones near the end of the sample.
            const size_t from = start(rng);
            for (int64_t k = 0; k < block && taken < n; ++k, ++taken) {
                sum += values[(from + static_cast<size_t>(k)) % n];
            }
        }
        means.push_back(sum / static_cast<double>(taken));
    }
    std::sort(means.begin(), means.end());
    const double alpha = (1.0 - confidence) / 2.0;
    const auto index = [&](double q) {
        const double pos = q * static_cast<double>(means.size() - 1);
        const size_t lo = static_cast<size_t>(pos);
        const size_t hi = std::min(lo + 1, means.size() - 1);
        return means[lo] * (1.0 - (pos - static_cast<double>(lo))) +
               means[hi] * (pos - static_cast<double>(lo));
    };
    out.lower = index(alpha);
    out.upper = index(1.0 - alpha);
    return out;
}

// Wilson score interval for a proportion. Chosen over the normal approximation
// because a win rate near 0 or 1 on a few dozen trades is exactly where the
// naive interval leaves the [0,1] range and stops meaning anything.
[[nodiscard]] inline Interval wilson_interval(int64_t successes, int64_t trials,
                                              double z = 1.959963985) {
    Interval out;
    out.samples = trials;
    if (trials <= 0) return out;
    const double n = static_cast<double>(trials);
    const double p = static_cast<double>(successes) / n;
    out.point = p;
    const double z2 = z * z;
    const double denom = 1.0 + z2 / n;
    const double centre = (p + z2 / (2.0 * n)) / denom;
    const double half = (z / denom) * std::sqrt(p * (1.0 - p) / n + z2 / (4.0 * n * n));
    out.lower = std::clamp(centre - half, 0.0, 1.0);
    out.upper = std::clamp(centre + half, 0.0, 1.0);
    return out;
}

// The uplift of the accepted set over the baseline, with an interval that
// accounts for both sets moving together under resampling: each replicate
// resamples the SAME blocks of candidates and recomputes both means from them,
// so the correlation between the two estimates is preserved.
[[nodiscard]] inline Interval block_bootstrap_uplift(const std::vector<double>& net_return,
                                                     const std::vector<char>& accepted,
                                                     double confidence = 0.95,
                                                     int replicates = 2000,
                                                     uint32_t seed = 20260823U) {
    Interval out;
    const size_t n = net_return.size();
    out.samples = static_cast<int64_t>(n);
    if (n == 0 || accepted.size() != n) return out;

    const auto uplift_of = [&](const std::vector<size_t>& idx) -> double {
        double accepted_sum = 0, all_sum = 0;
        for (const size_t i : idx) {
            all_sum += net_return[i];
            if (accepted[i] != 0) accepted_sum += net_return[i];
        }
        // Return per candidate OFFERED, for both sets, over the same draw.
        return (accepted_sum - all_sum) / static_cast<double>(idx.size());
    };
    std::vector<size_t> all(n);
    std::iota(all.begin(), all.end(), size_t{0});
    out.point = uplift_of(all);
    if (n < 8 || replicates < 100) return out;

    const int64_t block = block_bootstrap_length(n);
    out.block_length = block;
    const size_t blocks_per_replicate = (n + static_cast<size_t>(block) - 1) /
                                        static_cast<size_t>(block);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> start(0, n - 1);
    std::vector<double> draws;
    draws.reserve(static_cast<size_t>(replicates));
    std::vector<size_t> idx;
    idx.reserve(n);
    for (int r = 0; r < replicates; ++r) {
        idx.clear();
        for (size_t b = 0; b < blocks_per_replicate && idx.size() < n; ++b) {
            const size_t from = start(rng);
            for (int64_t k = 0; k < block && idx.size() < n; ++k) {
                idx.push_back((from + static_cast<size_t>(k)) % n);
            }
        }
        draws.push_back(uplift_of(idx));
    }
    std::sort(draws.begin(), draws.end());
    const double alpha = (1.0 - confidence) / 2.0;
    const auto index = [&](double q) {
        const double pos = q * static_cast<double>(draws.size() - 1);
        const size_t lo = static_cast<size_t>(pos);
        const size_t hi = std::min(lo + 1, draws.size() - 1);
        return draws[lo] * (1.0 - (pos - static_cast<double>(lo))) +
               draws[hi] * (pos - static_cast<double>(lo));
    };
    out.lower = index(alpha);
    out.upper = index(1.0 - alpha);
    return out;
}

} // namespace altsignalquality::domain
