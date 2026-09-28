#pragma once

// The leakage audit.
//
// Every claim this service makes about point-in-time correctness is checked
// here mechanically, and the checks run in three places: before every training
// cycle, in `altsignalquality validate`, and in the unit tests. A leakage
// finding is fatal — the cycle aborts and no model is registered — because a
// model trained on the future looks excellent and loses money.
//
// What is checked, and why each one can actually happen:
//
//   * contract availability — a feature added later with a source that is only
//     knowable after T0. Cheap to check, catastrophic to miss.
//   * label after observation — a row whose outcome matured at or before the
//     signal that produced it. Physically impossible, so its presence means the
//     maturation timestamp or the join is wrong.
//   * fold ordering — train strictly before validation strictly before test,
//     on observation time.
//   * maturation purge — no training row's label matured after its fold's
//     validation window opened, and no validation row's label matured after the
//     test window opened. This is the check that catches the failure mode
//     ordinary time-series splitting misses entirely.
//   * duplicate signals — one signal appearing in two slices, which would let
//     the model memorise a test row.

#include "altsignalquality/domain/feature_contract.hpp"
#include "altsignalquality/domain/split.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace altsignalquality::domain {

struct LeakageReport {
    std::vector<std::string> findings;
    [[nodiscard]] bool clean() const { return findings.empty(); }
};

// Every declared feature must be knowable at T0. The `availability` field is
// carried on the contract precisely so this is a loop and not a code review.
[[nodiscard]] inline LeakageReport audit_feature_contract() {
    LeakageReport r;
    for (size_t i = 0; i < kFeatures.size(); ++i) {
        if (kFeatures[i].availability != Availability::at_signal_time) {
            r.findings.push_back("feature " + std::to_string(i) + " (" +
                                 std::string(kFeatures[i].name) +
                                 ") is not declared AT_SIGNAL_TIME");
        }
    }
    // A duplicated name would make two positions indistinguishable in every
    // report, importance ranking and debugging session that follows.
    std::unordered_set<std::string> seen;
    for (const auto& f : kFeatures) {
        if (!seen.insert(std::string(f.name)).second) {
            r.findings.push_back("duplicate feature name: " + std::string(f.name));
        }
    }
    return r;
}

// Row-level invariants over the assembled dataset.
[[nodiscard]] inline LeakageReport audit_rows(const std::vector<RowTimes>& times,
                                              const std::vector<std::string>& signal_ids) {
    LeakageReport r;
    if (times.size() != signal_ids.size()) {
        r.findings.emplace_back("row time and signal id arrays have different lengths");
        return r;
    }
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < times.size(); ++i) {
        if (times[i].matured_ms <= times[i].observed_ms) {
            r.findings.push_back("row " + std::to_string(i) + " (" + signal_ids[i] +
                                 ") has a label that matured at or before its signal time");
        }
        if (times[i].observed_ms <= 0 || times[i].matured_ms <= 0) {
            r.findings.push_back("row " + std::to_string(i) + " (" + signal_ids[i] +
                                 ") has a non-positive time anchor");
        }
        if (!seen.insert(signal_ids[i]).second) {
            r.findings.push_back("signal " + signal_ids[i] + " appears in the dataset twice");
        }
    }
    return r;
}

namespace detail {

inline void check_slice(LeakageReport& r, const std::vector<RowTimes>& times,
                        const std::vector<int64_t>& idx, const std::string& what,
                        int64_t observed_below, int64_t observed_at_or_above,
                        int64_t matured_at_or_below) {
    for (const int64_t i : idx) {
        const auto& t = times[static_cast<size_t>(i)];
        if (t.observed_ms >= observed_below) {
            r.findings.push_back(what + " row " + std::to_string(i) +
                                 " was observed at or after its slice boundary");
        }
        if (t.observed_ms < observed_at_or_above) {
            r.findings.push_back(what + " row " + std::to_string(i) +
                                 " was observed before its slice boundary");
        }
        if (t.matured_ms > matured_at_or_below) {
            r.findings.push_back(what + " row " + std::to_string(i) +
                                 " carries a label that matured after the following window opened");
        }
    }
}

} // namespace detail

// Fold-level invariants. `embargo_ms` must be the same value the splitter used.
[[nodiscard]] inline LeakageReport audit_folds(const std::vector<RowTimes>& times,
                                               const std::vector<Fold>& folds,
                                               int64_t embargo_ms) {
    LeakageReport r;
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    for (size_t k = 0; k < folds.size(); ++k) {
        const auto& f = folds[k];
        const std::string tag = "fold " + std::to_string(k);
        if (!(f.valid_start_ms < f.test_start_ms && f.test_start_ms <= f.test_stop_ms)) {
            r.findings.push_back(tag + " has non-monotone window boundaries");
        }
        detail::check_slice(r, times, f.train, tag + " train", f.valid_start_ms, kMin,
                            f.valid_start_ms - embargo_ms);
        detail::check_slice(r, times, f.valid, tag + " valid", f.test_start_ms, f.valid_start_ms,
                            f.test_start_ms - embargo_ms);
        detail::check_slice(r, times, f.test, tag + " test", f.test_stop_ms, f.test_start_ms, kMax);

        std::unordered_set<int64_t> in_train(f.train.begin(), f.train.end());
        for (const int64_t i : f.valid) {
            if (in_train.count(i) != 0) {
                r.findings.push_back(tag + " row " + std::to_string(i) +
                                     " is in both train and validation");
            }
        }
        for (const int64_t i : f.test) {
            if (in_train.count(i) != 0) {
                r.findings.push_back(tag + " row " + std::to_string(i) +
                                     " is in both train and test");
            }
        }
    }
    return r;
}

[[nodiscard]] inline LeakageReport audit_final_split(const std::vector<RowTimes>& times,
                                                     const FinalSplit& split,
                                                     int64_t embargo_ms) {
    LeakageReport r;
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    detail::check_slice(r, times, split.train, "final train", split.valid_start_ms, kMin,
                        split.valid_start_ms - embargo_ms);
    detail::check_slice(r, times, split.valid, "final valid", split.test_start_ms,
                        split.valid_start_ms, split.test_start_ms - embargo_ms);
    // The refit slice is everything leakage-safe before the test window; it
    // deliberately spans train and validation, so only the test boundary applies.
    detail::check_slice(r, times, split.fit, "final fit", split.test_start_ms, kMin,
                        split.test_start_ms - embargo_ms);
    detail::check_slice(r, times, split.test, "final test", kMax, split.test_start_ms, kMax);

    std::unordered_set<int64_t> in_fit(split.fit.begin(), split.fit.end());
    for (const int64_t i : split.test) {
        if (in_fit.count(i) != 0) {
            r.findings.push_back("final split row " + std::to_string(i) +
                                 " is in both the refit and the held-out test slice");
        }
    }
    return r;
}

inline void merge(LeakageReport& into, const LeakageReport& from) {
    into.findings.insert(into.findings.end(), from.findings.begin(), from.findings.end());
}

} // namespace altsignalquality::domain
