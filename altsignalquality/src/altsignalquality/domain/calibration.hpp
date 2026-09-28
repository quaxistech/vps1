#pragma once

// Probability calibration: making `quality_score = 0.80` mean "about 80% of
// these actually resolve positive".
//
// A gradient-boosted binary model's raw output ranks well but is not a
// probability: its scores pile up wherever the loss is flat, so a threshold set
// at 0.8 on raw output is an arbitrary rank cut, not a confidence statement.
// The threshold in this service is an operational dial an engineer reasons
// about, and the score is published to VPS2, so it must mean what it says.
//
// The fit is isotonic regression by the Pool Adjacent Violators Algorithm on
// the VALIDATION slice only (never on test, never on the fit rows the booster
// already saw). Isotonic is chosen over Platt scaling because it is
// monotone-but-free-form — it can fix an S-shaped miscalibration that a single
// logistic squeeze cannot — and because it is exact, deterministic and needs no
// optimiser. The cost is that it can overfit small samples, so the trainer
// falls back to `identity` below a configured minimum and records which was
// used in `calibration.method`.
//
// The result is a monotone step function stored as knots, applied by binary
// search: cheap, allocation-free and identical on every host.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace altsignalquality::domain {

struct Calibration {
    // Step function: for a raw score s, the calibrated value is y[i] where i is
    // the last knot with x[i] <= s (and y[0] below the first knot). Both are
    // non-decreasing by construction.
    std::vector<double> x;
    std::vector<double> y;
    std::string method = "identity";
    std::string version;

    [[nodiscard]] bool usable() const {
        return method == "isotonic" && !x.empty() && x.size() == y.size();
    }

    [[nodiscard]] double apply(double raw) const {
        if (!usable()) return std::clamp(raw, 0.0, 1.0);
        if (!std::isfinite(raw)) return std::clamp(y.front(), 0.0, 1.0);
        const auto it = std::upper_bound(x.begin(), x.end(), raw);
        if (it == x.begin()) return std::clamp(y.front(), 0.0, 1.0);
        const size_t i = static_cast<size_t>(it - x.begin()) - 1;
        return std::clamp(y[i], 0.0, 1.0);
    }
};

// PAVA isotonic regression of `labels` on `scores`. Returns the pooled blocks
// as knots. Deterministic: ties in `scores` are pooled together, so the result
// does not depend on the input order.
[[nodiscard]] inline Calibration fit_isotonic(const std::vector<double>& scores,
                                              const std::vector<int>& labels,
                                              std::string version) {
    Calibration cal;
    cal.version = std::move(version);
    if (scores.size() != labels.size() || scores.empty()) return cal;  // identity

    std::vector<size_t> order(scores.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return scores[a] < scores[b]; });

    // A block of observations that will share one calibrated value.
    struct Block {
        double x = 0;       // the block's lowest raw score
        double weight = 0;
        double sum = 0;
        [[nodiscard]] double mean() const { return weight > 0 ? sum / weight : 0.0; }
    };

    // Ties are pooled BEFORE the PAVA pass. Two observations with the same raw
    // score must receive the same calibrated score; pooling them only when a
    // violation happens to merge them would make the answer depend on where the
    // block boundaries landed, and could hand identical inputs different
    // probabilities.
    std::vector<Block> blocks;
    for (const size_t i : order) {
        const double s = scores[i];
        if (!blocks.empty() && blocks.back().x == s) {
            blocks.back().weight += 1.0;
            blocks.back().sum += static_cast<double>(labels[i]);
        } else {
            blocks.push_back(Block{s, 1.0, static_cast<double>(labels[i])});
        }
    }

    // Pool adjacent violators: repeatedly merge any block whose mean is below
    // its predecessor's, until the sequence of means is non-decreasing.
    std::vector<Block> pooled;
    pooled.reserve(blocks.size());
    for (const auto& b : blocks) {
        pooled.push_back(b);
        while (pooled.size() > 1 && pooled[pooled.size() - 2].mean() > pooled.back().mean()) {
            Block merged = pooled[pooled.size() - 2];
            merged.weight += pooled.back().weight;
            merged.sum += pooled.back().sum;
            pooled.pop_back();
            pooled.back() = merged;
        }
    }

    cal.x.reserve(pooled.size());
    cal.y.reserve(pooled.size());
    for (const auto& b : pooled) {
        cal.x.push_back(b.x);
        cal.y.push_back(std::clamp(b.mean(), 0.0, 1.0));
    }
    cal.method = "isotonic";
    return cal;
}

[[nodiscard]] inline std::vector<double> apply_calibration(const Calibration& cal,
                                                           const std::vector<double>& raw) {
    std::vector<double> out;
    out.reserve(raw.size());
    for (const double r : raw) out.push_back(cal.apply(r));
    return out;
}

} // namespace altsignalquality::domain
