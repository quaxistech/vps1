// Chronological splitting with a maturation purge. The purge is the part that
// ordinary time-series splitting gets wrong here, so most of these tests are
// about a training row whose LABEL — not whose features — lands in the future.

#include "altsignalquality/domain/split.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iterator>
#include <limits>

using namespace altsignalquality::domain;

namespace {

constexpr int64_t kHour = 3'600'000;

// `n` signals one hour apart, each resolving `maturation_hours` later.
std::vector<RowTimes> series(size_t n, int64_t maturation_hours) {
    std::vector<RowTimes> rows;
    rows.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const int64_t t = static_cast<int64_t>(i + 1) * kHour;
        rows.push_back({t, t + maturation_hours * kHour});
    }
    return rows;
}

int64_t max_observed(const std::vector<RowTimes>& rows, const std::vector<int64_t>& idx) {
    int64_t out = 0;
    for (const int64_t i : idx) out = std::max(out, rows[static_cast<size_t>(i)].observed_ms);
    return out;
}

int64_t min_observed(const std::vector<RowTimes>& rows, const std::vector<int64_t>& idx) {
    int64_t out = std::numeric_limits<int64_t>::max();
    for (const int64_t i : idx) out = std::min(out, rows[static_cast<size_t>(i)].observed_ms);
    return out;
}

} // namespace

TEST_CASE("folds are strictly ordered in time") {
    const auto rows = series(400, 1);
    const auto folds = build_walk_forward_folds(rows, 3, 0);
    REQUIRE(folds.size() == 3);
    for (const auto& f : folds) {
        REQUIRE_FALSE(f.train.empty());
        REQUIRE_FALSE(f.valid.empty());
        REQUIRE_FALSE(f.test.empty());
        REQUIRE(max_observed(rows, f.train) < f.valid_start_ms);
        REQUIRE(min_observed(rows, f.valid) >= f.valid_start_ms);
        REQUIRE(max_observed(rows, f.valid) < f.test_start_ms);
        REQUIRE(min_observed(rows, f.test) >= f.test_start_ms);
    }
    // Expanding windows: each fold's test period starts after the previous one's.
    for (size_t k = 1; k < folds.size(); ++k) {
        REQUIRE(folds[k].test_start_ms > folds[k - 1].test_start_ms);
    }
}

TEST_CASE("a training row whose label matures inside the validation window is purged") {
    // Every signal here resolves 48 hours later — the stack's hard holding
    // limit. Splitting on observation time alone would put dozens of rows in
    // training whose outcome was only knowable well inside validation.
    const auto rows = series(400, 48);
    const auto folds = build_walk_forward_folds(rows, 3, 0);
    for (const auto& f : folds) {
        for (const int64_t i : f.train) {
            INFO("train row " << i);
            REQUIRE(rows[static_cast<size_t>(i)].matured_ms <= f.valid_start_ms);
        }
        for (const int64_t i : f.valid) {
            INFO("valid row " << i);
            REQUIRE(rows[static_cast<size_t>(i)].matured_ms <= f.test_start_ms);
        }
    }
}

TEST_CASE("the purge actually removes rows a naive split would have kept") {
    const auto quick = series(400, 1);
    const auto slow = series(400, 48);
    const auto quick_folds = build_walk_forward_folds(quick, 3, 0);
    const auto slow_folds = build_walk_forward_folds(slow, 3, 0);
    // Same observation times, same boundaries — only the maturation lag differs.
    REQUIRE(quick_folds[0].valid_start_ms == slow_folds[0].valid_start_ms);
    REQUIRE(slow_folds[0].train.size() < quick_folds[0].train.size());
}

TEST_CASE("the embargo widens the gap beyond the maturation purge") {
    const auto rows = series(400, 1);
    const auto none = build_walk_forward_folds(rows, 3, 0);
    const auto embargoed = build_walk_forward_folds(rows, 3, 12 * kHour);
    REQUIRE(embargoed[0].train.size() < none[0].train.size());
    for (const int64_t i : embargoed[0].train) {
        REQUIRE(rows[static_cast<size_t>(i)].matured_ms <=
                embargoed[0].valid_start_ms - 12 * kHour);
    }
}

TEST_CASE("the final split holds out a test slice the refit never sees") {
    const auto rows = series(500, 24);
    const auto split = build_final_split(rows, 3, 6 * kHour, 0.25);
    REQUIRE_FALSE(split.train.empty());
    REQUIRE_FALSE(split.valid.empty());
    REQUIRE_FALSE(split.fit.empty());
    REQUIRE_FALSE(split.test.empty());

    std::vector<int64_t> fit = split.fit, test = split.test;
    std::sort(fit.begin(), fit.end());
    std::sort(test.begin(), test.end());
    std::vector<int64_t> overlap;
    std::set_intersection(fit.begin(), fit.end(), test.begin(), test.end(),
                          std::back_inserter(overlap));
    REQUIRE(overlap.empty());

    // The refit spans train and validation, and is still purged at the test edge.
    REQUIRE(split.fit.size() >= split.train.size());
    for (const int64_t i : split.fit) {
        REQUIRE(rows[static_cast<size_t>(i)].matured_ms <= split.test_start_ms - 6 * kHour);
    }
    for (const int64_t i : split.test) {
        REQUIRE(rows[static_cast<size_t>(i)].observed_ms >= split.test_start_ms);
    }
}

TEST_CASE("too little history is refused rather than silently reshaped") {
    REQUIRE_THROWS_AS(build_walk_forward_folds(series(4, 1), 3, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(build_walk_forward_folds({}, 3, 0), std::invalid_argument);
    // A maturation lag wide enough to empty a slice is an error, not an empty fit.
    REQUIRE_THROWS_AS(build_walk_forward_folds(series(60, 10'000), 3, 0), std::invalid_argument);
}

TEST_CASE("splitting is deterministic") {
    const auto rows = series(300, 12);
    const auto a = build_walk_forward_folds(rows, 3, kHour);
    const auto b = build_walk_forward_folds(rows, 3, kHour);
    for (size_t k = 0; k < a.size(); ++k) {
        REQUIRE(a[k].train == b[k].train);
        REQUIRE(a[k].valid == b[k].valid);
        REQUIRE(a[k].test == b[k].test);
    }
}

// ---------------------------------------------------------------- regressions

// Production reproduction. AltSignalQuality recorded six FAILED training cycles
// reading "fold 0 is empty after the maturation purge and embargo". Nothing was
// broken: labels matured 8h after the signal, the embargo was 12h, and the
// archive spanned 113.6h, so the first of six segments (15.3h) was shorter than
// the 20h a training row needs to clear both. No arrangement of that data
// satisfies the contract, which makes it a state to wait out, not a fault.
TEST_CASE("a history too short for the purge and embargo defers, it does not fail") {
    // 120 signals an hour apart, each resolving 8h later; 12h embargo.
    const auto rows = series(120, 8);
    REQUIRE_THROWS_AS(build_walk_forward_folds(rows, 3, 12 * kHour, 30), InsufficientHistory);

    // Same shape, enough history: six segments of 20h+ each.
    const auto grown = series(6 * 21, 8);
    REQUIRE_NOTHROW(build_walk_forward_folds(grown, 3, 12 * kHour, 1));
}

// A genuinely malformed call stays a fault, so the trainer keeps recording it
// as FAILED rather than quietly retrying forever.
TEST_CASE("malformed split arguments remain faults, not deferrals") {
    const auto rows = series(400, 1);
    REQUIRE_THROWS_AS(build_walk_forward_folds({}, 3, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(build_walk_forward_folds(rows, 3, -1), std::invalid_argument);
    REQUIRE_THROWS_AS(build_walk_forward_folds(rows, 0, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(build_final_split(rows, 3, 0, 0.0), std::invalid_argument);
    // ... and none of those are the "wait for more data" signal.
    const auto is_history = [](const std::exception& e) {
        return dynamic_cast<const InsufficientHistory*>(&e) != nullptr;
    };
    try {
        (void)build_walk_forward_folds(rows, 3, -1);
    } catch (const std::exception& e) {
        REQUIRE_FALSE(is_history(e));
    }
}

// Checking a fold only for emptiness let two training rows through. LightGBM
// cannot place a split below min_data_in_leaf, so the booster is a constant and
// the per-fold AUC on the matching one-row validation slice is undefined; the
// gates then reject a model that was never trained, which reads as a verdict on
// merit instead of an immature dataset.
TEST_CASE("a fold below one leaf of training rows is not viable") {
    const auto rows = series(400, 1);
    const auto folds = build_walk_forward_folds(rows, 3, 0, 1);
    REQUIRE(folds.size() == 3);
    const auto smallest = folds.front().train.size();

    // Asking for more training rows than the first fold can supply defers.
    REQUIRE_THROWS_AS(build_walk_forward_folds(rows, 3, 0, smallest + 1), InsufficientHistory);
    // Asking for exactly what it supplies does not.
    REQUIRE_NOTHROW(build_walk_forward_folds(rows, 3, 0, smallest));
    // The bound applies to the final split's fit slice too.
    REQUIRE_THROWS_AS(build_final_split(rows, 3, 0, 0.25, rows.size() + 1), InsufficientHistory);
}
