#pragma once

// Chronological (walk-forward) splitting with a MATURATION purge.
//
// The ordinary time-series rule — "train on the past, test on the future" — is
// not sufficient here, because a signal's features exist at T0 but its LABEL
// only exists when the outcome matures, up to the 48h hard holding limit later.
// A training row observed before the validation window can therefore still
// carry a label that was only knowable inside it. Splitting on observation time
// alone would leak that.
//
// So every boundary is enforced twice:
//
//   train rows : observed before the validation window opens AND their label
//                matured before it opens (minus an embargo);
//   valid rows : observed inside the validation window AND their label matured
//                before the TEST window opens (minus the embargo) — otherwise
//                a threshold or a calibration fitted on validation would be
//                fitted on information from the test period;
//   test rows  : observed inside the test window, untouched until the single
//                final evaluation.
//
// The embargo is an extra gap on top of the maturation purge, for the residual
// autocorrelation between adjacent signals on the same instrument.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace altsignalquality::domain {

// The archive does not yet span enough time to build a leakage-safe split.
//
// This is a STATE, not a fault. Every boundary below is enforced twice (the
// maturation purge, then the embargo on top of it), so the earliest fold can
// only draw training rows from the stretch of history that closes more than
// `maturation + embargo` before the first validation window opens. Until the
// archive is longer than that, no arrangement of the data satisfies the
// contract, and the honest report is "not enough history yet".
//
// Derived from std::invalid_argument so callers that only distinguish "the
// split refused" keep working; the trainer catches this type first and records
// a DEFERRED cycle instead of a FAILED one.
struct InsufficientHistory : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

// One row's two time anchors. `index` refers back into the caller's arrays.
struct RowTimes {
    int64_t observed_ms = 0;
    int64_t matured_ms = 0;
};

struct Fold {
    std::vector<int64_t> train;
    std::vector<int64_t> valid;
    std::vector<int64_t> test;
    int64_t valid_start_ms = 0;
    int64_t test_start_ms = 0;
    int64_t test_stop_ms = 0;
};

struct FinalSplit {
    std::vector<int64_t> train;  // tuning fit
    std::vector<int64_t> valid;  // early stopping, threshold, calibration
    std::vector<int64_t> fit;    // full refit: everything leakage-safe before test
    std::vector<int64_t> test;   // held out, touched exactly once
    int64_t valid_start_ms = 0;
    int64_t test_start_ms = 0;
};

[[nodiscard]] inline std::vector<int64_t> sorted_unique_observations(
    const std::vector<RowTimes>& rows) {
    std::vector<int64_t> t;
    t.reserve(rows.size());
    for (const auto& r : rows) t.push_back(r.observed_ms);
    std::sort(t.begin(), t.end());
    t.erase(std::unique(t.begin(), t.end()), t.end());
    return t;
}

// n_folds + 4 evenly spaced boundaries over the distinct observation times,
// reserving the last segment for the final held-out test. Same protocol as
// AltML's trainer, so the two services' walk-forward reports are comparable.
[[nodiscard]] inline std::vector<size_t> split_edges(size_t n_unique, size_t n_folds) {
    if (n_folds < 1) throw std::invalid_argument("n_folds must be at least 1");
    if (n_unique < n_folds + 4) {
        throw InsufficientHistory("not enough distinct signal times for walk-forward folds: " +
                                  std::to_string(n_unique) + " of " +
                                  std::to_string(n_folds + 4) + " required");
    }
    const size_t k = n_folds + 4;
    std::vector<size_t> edges(k);
    for (size_t i = 0; i < k; ++i) {
        edges[i] = static_cast<size_t>(static_cast<double>(i) * static_cast<double>(n_unique) /
                                       static_cast<double>(k - 1));
    }
    for (size_t i = 1; i < k; ++i) {
        if (edges[i] <= edges[i - 1]) {
            throw InsufficientHistory("walk-forward boundaries collapsed: too few distinct "
                                      "signal times for " + std::to_string(n_folds) + " folds");
        }
    }
    return edges;
}

// Rows observed in [lo, hi) whose label had matured by `matured_before` (use
// INT64_MAX to disable the maturation purge, as for the untouched test slice).
[[nodiscard]] inline std::vector<int64_t> select_rows(const std::vector<RowTimes>& rows,
                                                      int64_t lo_inclusive,
                                                      int64_t hi_exclusive,
                                                      int64_t matured_before) {
    std::vector<int64_t> out;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (r.observed_ms < lo_inclusive || r.observed_ms >= hi_exclusive) continue;
        if (r.matured_ms > matured_before) continue;
        out.push_back(static_cast<int64_t>(i));
    }
    return out;
}

// `min_train_rows` is the smallest training slice the learner can actually use.
// Checking only for emptiness let a fold through with a handful of rows: the
// booster cannot place a single split below `min_data_in_leaf` and returns a
// constant, per-fold AUC is undefined on the matching one-row validation slice,
// and the gates then reject a "model" that was never trained on anything. That
// reads as a model rejected on merit; the truth is a dataset that has not grown
// enough yet, which is why falling short defers rather than fails.
[[nodiscard]] inline std::vector<Fold> build_walk_forward_folds(const std::vector<RowTimes>& rows,
                                                                size_t n_folds,
                                                                int64_t embargo_ms,
                                                                size_t min_train_rows = 1) {
    if (rows.empty()) throw std::invalid_argument("empty row times");
    if (embargo_ms < 0) throw std::invalid_argument("negative embargo");
    if (min_train_rows < 1) throw std::invalid_argument("min_train_rows must be at least 1");
    const auto unique = sorted_unique_observations(rows);
    const auto edges = split_edges(unique.size(), n_folds);
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

    std::vector<Fold> folds;
    for (size_t k = 0; k < n_folds; ++k) {
        Fold f;
        f.valid_start_ms = unique[edges[k + 1]];
        f.test_start_ms = unique[edges[k + 2]];
        f.test_stop_ms = edges[k + 3] < unique.size() ? unique[edges[k + 3]] : kMax;
        f.train = select_rows(rows, kMin, f.valid_start_ms, f.valid_start_ms - embargo_ms);
        f.valid = select_rows(rows, f.valid_start_ms, f.test_start_ms,
                              f.test_start_ms - embargo_ms);
        f.test = select_rows(rows, f.test_start_ms, f.test_stop_ms, kMax);
        if (f.train.size() < min_train_rows || f.valid.empty() || f.test.empty()) {
            throw InsufficientHistory(
                "fold " + std::to_string(k) + " is not viable after the maturation purge and " +
                std::to_string(embargo_ms / 60000) + "min embargo: train=" +
                std::to_string(f.train.size()) + " (minimum " + std::to_string(min_train_rows) +
                "), valid=" + std::to_string(f.valid.size()) +
                ", test=" + std::to_string(f.test.size()));
        }
        folds.push_back(std::move(f));
    }
    return folds;
}

[[nodiscard]] inline FinalSplit build_final_split(const std::vector<RowTimes>& rows,
                                                  size_t n_folds, int64_t embargo_ms,
                                                  double validation_fraction,
                                                  size_t min_train_rows = 1) {
    if (validation_fraction <= 0.0 || validation_fraction >= 0.9) {
        throw std::invalid_argument("validation_fraction must be in (0, 0.9)");
    }
    if (min_train_rows < 1) throw std::invalid_argument("min_train_rows must be at least 1");
    const auto unique = sorted_unique_observations(rows);
    const auto edges = split_edges(unique.size(), n_folds);
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

    FinalSplit split;
    split.test_start_ms = unique[edges[n_folds + 2]];

    // Candidate validation starts: observation times that are themselves
    // leakage-safe with respect to the test boundary.
    std::vector<int64_t> pretest;
    for (const int64_t u : unique) {
        if (u < split.test_start_ms - embargo_ms) pretest.push_back(u);
    }
    if (pretest.size() < 3) {
        throw InsufficientHistory("not enough pre-test signal times after the embargo: " +
                                  std::to_string(pretest.size()) + " of 3 required");
    }
    size_t pos = static_cast<size_t>(static_cast<double>(pretest.size()) *
                                     (1.0 - validation_fraction));
    pos = std::min(std::max<size_t>(pos, 1), pretest.size() - 1);
    split.valid_start_ms = pretest[pos];

    split.train = select_rows(rows, kMin, split.valid_start_ms,
                              split.valid_start_ms - embargo_ms);
    split.valid = select_rows(rows, split.valid_start_ms, split.test_start_ms,
                              split.test_start_ms - embargo_ms);
    split.fit = select_rows(rows, kMin, split.test_start_ms, split.test_start_ms - embargo_ms);
    split.test = select_rows(rows, split.test_start_ms, kMax, kMax);
    if (split.train.size() < min_train_rows || split.fit.size() < min_train_rows ||
        split.valid.empty() || split.test.empty()) {
        throw InsufficientHistory(
            "final split is not viable after the maturation purge and " +
            std::to_string(embargo_ms / 60000) + "min embargo: train=" +
            std::to_string(split.train.size()) + ", fit=" + std::to_string(split.fit.size()) +
            " (minimum " + std::to_string(min_train_rows) +
            "), valid=" + std::to_string(split.valid.size()) +
            ", test=" + std::to_string(split.test.size()));
    }
    return split;
}

} // namespace altsignalquality::domain
