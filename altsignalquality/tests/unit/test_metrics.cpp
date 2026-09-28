// Filter metrics. Accuracy is deliberately absent from this service; what these
// tests pin is that the accepted set is always measured AGAINST the baseline of
// taking every signal, and that the degenerate filters score as badly as they
// deserve to.

#include "altsignalquality/domain/metrics.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace altsignalquality::domain;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("ROC-AUC is 1 for a perfect ranking and undefined for one class") {
    REQUIRE_THAT(roc_auc({0, 0, 1, 1}, {0.1, 0.2, 0.8, 0.9}), WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(roc_auc({1, 1, 0, 0}, {0.1, 0.2, 0.8, 0.9}), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(roc_auc({0, 1}, {0.5, 0.5}), WithinAbs(0.5, 1e-12));
    REQUIRE(std::isnan(roc_auc({1, 1, 1}, {0.1, 0.2, 0.3})));
    REQUIRE(std::isnan(roc_auc({}, {})));
}

TEST_CASE("average precision rewards putting the positives first") {
    REQUIRE_THAT(pr_auc({1, 1, 0, 0}, {0.9, 0.8, 0.2, 0.1}), WithinAbs(1.0, 1e-12));
    const double mixed = pr_auc({1, 0, 1, 0}, {0.9, 0.8, 0.7, 0.1});
    REQUIRE(mixed > 0.5);
    REQUIRE(mixed < 1.0);
    REQUIRE(std::isnan(pr_auc({0, 0}, {0.1, 0.2})));
}

TEST_CASE("expected calibration error is zero for a perfectly calibrated set") {
    // 10 samples at p = 0.5, exactly 5 of which are wins.
    std::vector<int> y;
    std::vector<double> p;
    for (int i = 0; i < 10; ++i) {
        y.push_back(i % 2);
        p.push_back(0.5);
    }
    REQUIRE_THAT(expected_calibration_error(y, p), WithinAbs(0.0, 1e-12));

    // A model that says 0.9 and is right half the time is off by 0.4.
    std::vector<int> overconfident_y;
    std::vector<double> overconfident_p;
    for (int i = 0; i < 10; ++i) {
        overconfident_y.push_back(i % 2);
        overconfident_p.push_back(0.9);
    }
    REQUIRE_THAT(expected_calibration_error(overconfident_y, overconfident_p),
                 WithinAbs(0.4, 1e-12));
}

TEST_CASE("the filter evaluation reports the accepted set next to the baseline") {
    //                     losers            winners
    const std::vector<int> y = {0, 0, 0, 1, 1, 1};
    const std::vector<double> score = {0.1, 0.2, 0.3, 0.7, 0.8, 0.9};
    const std::vector<double> net = {-0.01, -0.01, -0.01, 0.02, 0.02, 0.02};

    const auto ev = evaluate_filter(y, score, net, 0.5);
    REQUIRE(ev.accepted.count == 3);
    REQUIRE_THAT(ev.acceptance_rate, WithinAbs(0.5, 1e-12));
    REQUIRE_THAT(ev.accepted.win_rate, WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(ev.accepted.expectancy, WithinAbs(0.02, 1e-12));
    REQUIRE_THAT(ev.baseline.win_rate, WithinAbs(0.5, 1e-12));
    REQUIRE_THAT(ev.baseline.expectancy, WithinAbs(0.005, 1e-12));
    REQUIRE_THAT(ev.win_rate_uplift, WithinAbs(0.5, 1e-12));
    // Return per candidate OFFERED: 0.06 total over 6 candidates.
    REQUIRE_THAT(ev.accepted.return_per_candidate, WithinAbs(0.01, 1e-12));
    REQUIRE_THAT(ev.baseline.return_per_candidate, WithinAbs(0.005, 1e-12));
    REQUIRE_THAT(ev.return_per_candidate_uplift, WithinAbs(0.005, 1e-12));
    // What it declined matters too: the rejected set was pure loss.
    REQUIRE(ev.rejected.count == 3);
    REQUIRE_THAT(ev.rejected.expectancy, WithinAbs(-0.01, 1e-12));
}

TEST_CASE("a filter that accepts nothing earns nothing") {
    const std::vector<int> y = {0, 1};
    const std::vector<double> score = {0.1, 0.2};
    const std::vector<double> net = {-0.01, 0.02};
    const auto ev = evaluate_filter(y, score, net, 0.99);
    REQUIRE(ev.accepted.count == 0);
    REQUIRE_THAT(ev.accepted.return_per_candidate, WithinAbs(0.0, 1e-12));
    REQUIRE(std::isnan(ev.accepted.expectancy));
    // Which is worse than the baseline, and the uplift says so.
    REQUIRE(ev.return_per_candidate_uplift < 0);
}

TEST_CASE("accepting everything scores exactly the baseline, with zero uplift") {
    const std::vector<int> y = {0, 1, 1};
    const std::vector<double> score = {0.6, 0.7, 0.8};
    const std::vector<double> net = {-0.01, 0.02, 0.03};
    const auto ev = evaluate_filter(y, score, net, 0.0);
    REQUIRE(ev.accepted.count == 3);
    REQUIRE_THAT(ev.acceptance_rate, WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(ev.return_per_candidate_uplift, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(ev.expectancy_uplift, WithinAbs(0.0, 1e-12));
}

TEST_CASE("a higher mean earned by trading almost nothing is not an improvement") {
    // The point of return-per-candidate: a filter that accepts one brilliant
    // signal has a superb MEAN and earns the desk far less than one that takes
    // every profitable signal available.
    //
    // The baseline here is genuinely profitable — 60 winners at +2%, 40 losers
    // at −1%, so +80 bps per candidate offered. Against that, creaming off the
    // single best signal is a large step backwards, however good its mean.
    std::vector<int> y;
    std::vector<double> score, net;
    y.push_back(1);
    score.push_back(0.99);
    net.push_back(0.02);
    for (int i = 0; i < 99; ++i) {
        const bool win = i % 5 < 3;  // 60% winners
        y.push_back(win ? 1 : 0);
        score.push_back(0.4);
        net.push_back(win ? 0.02 : -0.01);
    }
    const auto strict = evaluate_filter(y, score, net, 0.9);      // takes 1 of 100
    const auto permissive = evaluate_filter(y, score, net, 0.0);  // takes all 100

    REQUIRE(strict.accepted.count == 1);
    REQUIRE(permissive.accepted.count == 100);
    // The mean says the strict filter is better...
    REQUIRE(strict.accepted.expectancy > permissive.accepted.expectancy);
    // ...and the quantity the desk actually earns says the opposite, which is
    // why the threshold search optimises this one.
    REQUIRE(permissive.accepted.return_per_candidate > 0.0);
    REQUIRE(strict.accepted.return_per_candidate < permissive.accepted.return_per_candidate);
    // The uplift confirms it: creaming is worse than the baseline it replaced.
    REQUIRE(strict.return_per_candidate_uplift < 0.0);
}

TEST_CASE("stability finds the bucket the filter is actually harmful in") {
    std::vector<int> y;
    std::vector<double> score, net;
    std::vector<std::string> bucket;
    // In "good" the high scores win; in "bad" they lose.
    for (int i = 0; i < 60; ++i) {
        const bool good = i < 30;
        const bool high = (i % 2) == 0;
        bucket.emplace_back(good ? "good" : "bad");
        score.push_back(high ? 0.9 : 0.1);
        const bool win = good ? high : !high;
        y.push_back(win ? 1 : 0);
        net.push_back(win ? 0.02 : -0.02);
    }
    const auto s = stability_by_bucket(y, score, net, bucket, 0.5, 10);
    REQUIRE(s.buckets_evaluated == 2);
    REQUIRE(s.buckets_non_negative == 1);
    REQUIRE_THAT(s.non_negative_share, WithinAbs(0.5, 1e-12));
    REQUIRE(s.worst_bucket == "bad");
    REQUIRE(s.worst_uplift < 0);
}

TEST_CASE("buckets below the sample floor are not evaluated at all") {
    const std::vector<int> y = {1, 0};
    const std::vector<double> score = {0.9, 0.1};
    const std::vector<double> net = {0.02, -0.02};
    const std::vector<std::string> bucket = {"tiny", "tiny"};
    const auto s = stability_by_bucket(y, score, net, bucket, 0.5, 10);
    REQUIRE(s.buckets_evaluated == 0);
    REQUIRE(std::isnan(s.non_negative_share));
}
