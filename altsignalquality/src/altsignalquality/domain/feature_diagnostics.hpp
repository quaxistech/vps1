#pragma once

// What the features ACTUALLY contain in a given dataset.
//
// A contract declares 110 inputs; it does not make them informative. On this
// stack's own history, several of them are constant by construction upstream —
// AltStrategy hardcodes `p_timeout = 0.1` and `p_ambiguous = 0.05` whenever
// AltML's rich forecast is absent, which is currently always — and an entire
// 34-member block is empty on any deployment where AltStrategy does not persist
// its context snapshot.
//
// None of that is an error. Silently not knowing about it is. A zero-variance
// feature cannot help the model, an all-missing block means a whole information
// source is disconnected, and an exact duplicate quietly halves the effective
// feature-sampling budget. So every training cycle measures this, reports it,
// and logs it — the model's report says what the model was actually able to
// learn from.

#include "altsignalquality/domain/feature_contract.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace altsignalquality::domain {

struct FeatureStat {
    size_t position = 0;
    std::string name;
    int64_t observed = 0;       // rows where the value is present
    int64_t missing = 0;
    double mean = std::nan("");
    double stddev = std::nan("");
    double min_value = std::nan("");
    double max_value = std::nan("");
    int64_t distinct_estimate = 0;   // capped; enough to spot a constant
    bool constant = false;           // observed but never varies
    bool all_missing = false;
    std::string duplicate_of;        // an earlier feature identical in every row
};

struct FeatureDiagnostics {
    std::vector<FeatureStat> stats;
    int64_t informative = 0;
    int64_t constant = 0;
    int64_t all_missing = 0;
    int64_t duplicates = 0;
    [[nodiscard]] double informative_share() const {
        return stats.empty() ? std::nan("")
                             : static_cast<double>(informative) /
                                   static_cast<double>(stats.size());
    }
};

// `matrix` is row-major, `rows * kFeatureCount`. NaN means missing, which is
// exactly how the assembler encodes an unobserved input.
[[nodiscard]] inline FeatureDiagnostics diagnose_features(const std::vector<float>& matrix,
                                                          size_t rows) {
    FeatureDiagnostics out;
    const size_t n_feat = kFeatureCount;
    if (rows == 0 || matrix.size() != rows * n_feat) return out;

    out.stats.reserve(n_feat);
    for (size_t f = 0; f < n_feat; ++f) {
        FeatureStat st;
        st.position = f;
        st.name = std::string(kFeatures[f].name);
        // Welford's online variance, not sum-of-squares. Some of these
        // features carry values in the hundreds of millions (top-of-book depth
        // in USDT), where `sum_sq/n - mean^2` cancels catastrophically and can
        // report a variance of zero for a column that plainly varies — turning
        // a live feature into a "constant" in the very diagnostic meant to be
        // trusted.
        double mean = 0, m2 = 0;
        double lo = 0, hi = 0;
        bool first = true;
        // A distinct-value estimate that stops early: the question is "is this
        // constant / near-constant", not "how many levels exactly".
        std::vector<float> seen;
        seen.reserve(16);
        for (size_t r = 0; r < rows; ++r) {
            const float v = matrix[r * n_feat + f];
            if (!std::isfinite(v)) {
                ++st.missing;
                continue;
            }
            ++st.observed;
            const double d = v;
            const double delta = d - mean;
            mean += delta / static_cast<double>(st.observed);
            m2 += delta * (d - mean);
            if (first) {
                lo = hi = d;
                first = false;
            } else {
                lo = std::min(lo, d);
                hi = std::max(hi, d);
            }
            if (seen.size() < 16 &&
                std::find(seen.begin(), seen.end(), v) == seen.end()) {
                seen.push_back(v);
            }
        }
        st.distinct_estimate = static_cast<int64_t>(seen.size());
        if (st.observed > 0) {
            const double n = static_cast<double>(st.observed);
            st.mean = mean;
            st.stddev = n > 1.0 ? std::sqrt(std::max(0.0, m2 / n)) : 0.0;
            st.min_value = lo;
            st.max_value = hi;
            st.constant = st.min_value == st.max_value;
        } else {
            st.all_missing = true;
        }
        out.stats.push_back(std::move(st));
    }

    // Exact duplicates, compared over the full column including the missing
    // pattern. Only earlier-position matches are recorded, so a pair is
    // reported once and the survivor is the one the contract lists first.
    //
    // The full comparison is O(rows) per pair and there are F²/2 pairs, which
    // at 110 features and a few hundred thousand rows is billions of float
    // comparisons inside a training cycle. A cheap signature — the summary
    // statistics already computed above — rejects almost every pair in constant
    // time, and only survivors are compared column by column. The signature can
    // collide; the full comparison is what decides.
    const auto signature_matches = [](const FeatureStat& a, const FeatureStat& b) {
        const auto same = [](double x, double y) {
            return (std::isnan(x) && std::isnan(y)) || x == y;
        };
        return a.observed == b.observed && a.missing == b.missing &&
               a.distinct_estimate == b.distinct_estimate && same(a.mean, b.mean) &&
               same(a.stddev, b.stddev) && same(a.min_value, b.min_value) &&
               same(a.max_value, b.max_value);
    };
    for (size_t f = 0; f < n_feat; ++f) {
        // Two constants that happen to hold the same number are trivially
        // "identical", and reporting them as duplicates of each other buries
        // the one finding that matters — a genuine duplicate of a VARYING
        // quantity, which is what actually costs the model a feature slot.
        // Constants are already reported as constants.
        if (out.stats[f].all_missing || out.stats[f].constant) continue;
        for (size_t g = 0; g < f; ++g) {
            if (!out.stats[g].duplicate_of.empty() || out.stats[g].all_missing ||
                out.stats[g].constant) {
                continue;
            }
            if (!signature_matches(out.stats[f], out.stats[g])) continue;
            bool identical = true;
            for (size_t r = 0; r < rows && identical; ++r) {
                const float a = matrix[r * n_feat + f];
                const float b = matrix[r * n_feat + g];
                const bool a_missing = !std::isfinite(a);
                const bool b_missing = !std::isfinite(b);
                if (a_missing != b_missing) identical = false;
                else if (!a_missing && a != b) identical = false;
            }
            if (identical) {
                out.stats[f].duplicate_of = out.stats[g].name;
                break;
            }
        }
    }

    for (const auto& st : out.stats) {
        if (st.all_missing) ++out.all_missing;
        else if (st.constant) ++out.constant;
        else if (!st.duplicate_of.empty()) ++out.duplicates;
        else ++out.informative;
    }
    return out;
}

} // namespace altsignalquality::domain
