// The leakage audit must actually find leaks. Each test plants one and checks
// the audit reports it; a check that only passes on clean data is worthless.

#include "altsignalquality/domain/leakage.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace altsignalquality::domain;

namespace {

constexpr int64_t kHour = 3'600'000;

std::vector<RowTimes> series(size_t n, int64_t maturation_hours) {
    std::vector<RowTimes> rows;
    for (size_t i = 0; i < n; ++i) {
        const int64_t t = static_cast<int64_t>(i + 1) * kHour;
        rows.push_back({t, t + maturation_hours * kHour});
    }
    return rows;
}

std::vector<std::string> ids(size_t n) {
    std::vector<std::string> out;
    for (size_t i = 0; i < n; ++i) out.push_back("sig-" + std::to_string(i));
    return out;
}

bool mentions(const LeakageReport& r, std::string_view fragment) {
    for (const auto& f : r.findings) {
        if (f.find(fragment) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST_CASE("the shipped feature contract passes its own audit") {
    const auto r = audit_feature_contract();
    // Extra parentheses: INFO expands to `builder << expr`, and `<<` binds
    // tighter than `?:`, so an unparenthesised ternary is parsed as
    // `(builder << cond) ? a : b`.
    INFO((r.findings.empty() ? std::string() : r.findings.front()));
    REQUIRE(r.clean());
}

TEST_CASE("a label that matured before its signal is reported") {
    auto rows = series(10, 4);
    rows[3].matured_ms = rows[3].observed_ms - kHour;   // impossible
    const auto r = audit_rows(rows, ids(10));
    REQUIRE_FALSE(r.clean());
    REQUIRE(mentions(r, "matured at or before its signal time"));
}

TEST_CASE("a label that matured exactly at its signal time is reported") {
    auto rows = series(10, 4);
    rows[5].matured_ms = rows[5].observed_ms;
    REQUIRE(mentions(audit_rows(rows, ids(10)), "matured at or before"));
}

TEST_CASE("the same signal appearing twice is reported") {
    const auto rows = series(4, 4);
    std::vector<std::string> duplicated = {"a", "b", "a", "c"};
    const auto r = audit_rows(rows, duplicated);
    REQUIRE_FALSE(r.clean());
    REQUIRE(mentions(r, "appears in the dataset twice"));
}

TEST_CASE("a non-positive time anchor is reported") {
    auto rows = series(5, 4);
    rows[2].observed_ms = 0;
    REQUIRE(mentions(audit_rows(rows, ids(5)), "non-positive time anchor"));
}

TEST_CASE("clean data produces no findings") {
    REQUIRE(audit_rows(series(50, 6), ids(50)).clean());
}

TEST_CASE("a correctly built split passes the fold audit") {
    const auto rows = series(400, 24);
    const auto folds = build_walk_forward_folds(rows, 3, 2 * kHour);
    const auto split = build_final_split(rows, 3, 2 * kHour, 0.25);
    REQUIRE(audit_folds(rows, folds, 2 * kHour).clean());
    REQUIRE(audit_final_split(rows, split, 2 * kHour).clean());
}

TEST_CASE("a training row smuggled in from the future is caught") {
    const auto rows = series(400, 24);
    auto folds = build_walk_forward_folds(rows, 3, 0);
    // Add a row observed inside the validation window to the training slice —
    // precisely what a split on the wrong column would produce.
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].observed_ms >= folds[0].valid_start_ms) {
            folds[0].train.push_back(static_cast<int64_t>(i));
            break;
        }
    }
    const auto r = audit_folds(rows, folds, 0);
    REQUIRE_FALSE(r.clean());
    REQUIRE(mentions(r, "observed at or after its slice boundary"));
}

TEST_CASE("a training row whose LABEL lands in the validation window is caught") {
    // This is the failure a purely observation-time split cannot see: the
    // features are safely in the past, the outcome is not.
    const auto rows = series(400, 48);
    auto folds = build_walk_forward_folds(rows, 3, 0);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].observed_ms < folds[0].valid_start_ms &&
            rows[i].matured_ms > folds[0].valid_start_ms) {
            folds[0].train.push_back(static_cast<int64_t>(i));
            break;
        }
    }
    const auto r = audit_folds(rows, folds, 0);
    REQUIRE_FALSE(r.clean());
    REQUIRE(mentions(r, "matured after the following window opened"));
}

TEST_CASE("a row in both the refit and the held-out test slice is caught") {
    const auto rows = series(400, 12);
    auto split = build_final_split(rows, 3, kHour, 0.25);
    split.fit.push_back(split.test.front());
    const auto r = audit_final_split(rows, split, kHour);
    REQUIRE_FALSE(r.clean());
    REQUIRE(mentions(r, "both the refit and the held-out test slice"));
}

TEST_CASE("findings from several audits merge into one report") {
    LeakageReport combined;
    auto rows = series(10, 4);
    rows[1].matured_ms = rows[1].observed_ms;
    merge(combined, audit_feature_contract());
    merge(combined, audit_rows(rows, ids(10)));
    REQUIRE_FALSE(combined.clean());
    REQUIRE(combined.findings.size() >= 1);
}
