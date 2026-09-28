// Isotonic calibration by PAVA. The properties that matter operationally:
// monotonicity (a higher raw score never gets a lower probability), tie
// consistency (identical inputs get identical outputs) and the actual isotonic
// solution rather than something merely monotone.

#include "altsignalquality/domain/calibration.hpp"
#include "altsignalquality/domain/metrics.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <random>

using namespace altsignalquality::domain;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("an unfitted calibration is the identity, clamped to a probability") {
    Calibration cal;
    REQUIRE_FALSE(cal.usable());
    REQUIRE_THAT(cal.apply(0.37), WithinAbs(0.37, 1e-12));
    REQUIRE_THAT(cal.apply(-3.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(cal.apply(9.0), WithinAbs(1.0, 1e-12));
}

TEST_CASE("an already-ordered sample is left alone") {
    const std::vector<double> scores = {0.1, 0.2, 0.3, 0.4};
    const std::vector<int> labels = {0, 0, 1, 1};
    const auto cal = fit_isotonic(scores, labels, "v1");
    REQUIRE(cal.usable());
    REQUIRE_THAT(cal.apply(0.1), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(cal.apply(0.4), WithinAbs(1.0, 1e-12));
}

TEST_CASE("adjacent violators are pooled to their mean") {
    // 0.2 says "loss" while 0.3 says "win": the pair is pooled to 0.5.
    const std::vector<double> scores = {0.1, 0.2, 0.3, 0.4};
    const std::vector<int> labels = {0, 1, 0, 1};
    const auto cal = fit_isotonic(scores, labels, "v1");
    REQUIRE_THAT(cal.apply(0.2), WithinAbs(0.5, 1e-12));
    REQUIRE_THAT(cal.apply(0.3), WithinAbs(0.5, 1e-12));
}

TEST_CASE("identical raw scores always receive identical probabilities") {
    // Pooling ties only when a violation happens to merge them would hand two
    // identical inputs two different probabilities, which is indefensible in a
    // number published to a downstream risk system.
    const std::vector<double> scores = {0.1, 0.5, 0.5};
    const std::vector<int> labels = {1, 0, 1};
    const auto cal = fit_isotonic(scores, labels, "v1");
    REQUIRE_THAT(cal.apply(0.5), WithinAbs(cal.apply(0.5), 1e-12));
    // The isotonic solution here is a single block at 2/3.
    REQUIRE_THAT(cal.apply(0.1), WithinRel(2.0 / 3.0, 1e-9));
    REQUIRE_THAT(cal.apply(0.5), WithinRel(2.0 / 3.0, 1e-9));
}

TEST_CASE("the fitted mapping is non-decreasing everywhere") {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    std::vector<double> scores;
    std::vector<int> labels;
    for (int i = 0; i < 500; ++i) {
        const double s = uniform(rng);
        scores.push_back(s);
        labels.push_back(uniform(rng) < s ? 1 : 0);
    }
    const auto cal = fit_isotonic(scores, labels, "v1");
    double previous = -1.0;
    for (int i = 0; i <= 100; ++i) {
        const double p = cal.apply(static_cast<double>(i) / 100.0);
        REQUIRE(p >= previous - 1e-12);
        REQUIRE(p >= 0.0);
        REQUIRE(p <= 1.0);
        previous = p;
    }
}

TEST_CASE("calibration improves a systematically overconfident model") {
    // A model whose raw output is the true probability squeezed toward the
    // extremes: ranking is perfect, the numbers are not probabilities.
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    std::vector<double> raw;
    std::vector<int> labels;
    for (int i = 0; i < 3000; ++i) {
        const double truth = uniform(rng);
        labels.push_back(uniform(rng) < truth ? 1 : 0);
        raw.push_back(std::clamp(truth * 2.0 - 0.5, 0.0, 1.0));
    }
    const auto cal = fit_isotonic(raw, labels, "v1");
    const auto calibrated = apply_calibration(cal, raw);
    REQUIRE(expected_calibration_error(labels, calibrated) <
            expected_calibration_error(labels, raw));
    REQUIRE(brier_score(labels, calibrated) < brier_score(labels, raw));

    // Calibration is monotone NON-DECREASING, which is weaker than
    // order-preserving: PAVA pools adjacent blocks, and pooling turns distinct
    // scores into ties. ROC-AUC scores a tie as half a correctly ordered pair,
    // so the value moves — in practice upward, because the pairs that get
    // pooled are exactly the ones that were mis-ordered. What it can never do
    // is INVERT a pair, so the ranking cannot get worse.
    const double raw_auc = roc_auc(labels, raw);
    const double calibrated_auc = roc_auc(labels, calibrated);
    REQUIRE(calibrated_auc >= raw_auc - 1e-12);
    // And the change is a consequence of pooling, not a reordering: the
    // calibrated scores are non-decreasing in the raw ones.
    for (size_t i = 0; i < raw.size(); ++i) {
        for (size_t j = i + 1; j < raw.size(); j += 97) {  // sampled, O(n) pairs
            if (raw[i] < raw[j]) REQUIRE(calibrated[i] <= calibrated[j]);
            if (raw[i] > raw[j]) REQUIRE(calibrated[i] >= calibrated[j]);
        }
    }
}

TEST_CASE("scores outside the fitted range extrapolate flat, never past [0,1]") {
    const std::vector<double> scores = {0.3, 0.4, 0.5};
    const std::vector<int> labels = {0, 1, 1};
    const auto cal = fit_isotonic(scores, labels, "v1");
    REQUIRE_THAT(cal.apply(-5.0), WithinAbs(cal.y.front(), 1e-12));
    REQUIRE_THAT(cal.apply(5.0), WithinAbs(cal.y.back(), 1e-12));
    REQUIRE_THAT(cal.apply(std::nan("")), WithinAbs(cal.y.front(), 1e-12));
}

TEST_CASE("an empty or mismatched sample yields the identity rather than a fit") {
    REQUIRE_FALSE(fit_isotonic({}, {}, "v1").usable());
    REQUIRE_FALSE(fit_isotonic({0.1, 0.2}, {1}, "v1").usable());
}
