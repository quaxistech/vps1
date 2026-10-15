#include "altstrategy/engine/decision_types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace altstrategy::engine;

namespace {
// Templates, so a missing member is substitution failure rather than an error.
template <typename T> concept HasStopBeforeTarget = requires(T m) { m.p_stop_before_target; };
template <typename T> concept HasTimeout = requires(T m) { m.p_timeout; };

MarketForecastView good() {
    MarketForecastView f;
    f.present = true;
    f.abstain = false;
    f.p_up = 0.61;
    f.p_down = 0.39;
    f.probability_target_before_stop = 0.57;
    f.expected_return = 0.0042;
    f.expected_mfe_long_fraction = 0.0090;
    f.expected_mae_long_fraction = 0.0031;
    f.expected_holding_time_min = 95;
    f.conviction = 0.44;
    f.quality_status = "VALID";
    f.model_id = "direction";
    f.model_version = 3;
    return f;
}
} // namespace

TEST_CASE("an absent forecast is unavailable, not a coin flip") {
    MarketForecastView f;   // present = false
    const auto ml = ml_from_forecast(f);
    CHECK(ml.unavailable);
    CHECK_FALSE(ml.present);
    // The whole point: nothing invented a neutral probability.
    CHECK(ml.p_up == 0.0);
    CHECK(ml.p_down == 0.0);
    CHECK(ml.p_target_before_stop == 0.0);
    CHECK(ml.conviction == 0.0);
}

TEST_CASE("an abstaining forecast carries no probabilities") {
    auto f = good();
    f.abstain = true;
    const auto ml = ml_from_forecast(f);
    CHECK(ml.present);
    CHECK(ml.abstain);
    // AltML said "I decline to predict". Copying the numbers it emitted anyway
    // would turn an abstention into a prediction.
    CHECK(ml.p_up == 0.0);
    CHECK(ml.p_target_before_stop == 0.0);
    CHECK(ml.conviction == 0.0);
}

TEST_CASE("a real forecast reaches the evaluation unmodified") {
    const auto ml = ml_from_forecast(good());
    using Catch::Matchers::WithinAbs;
    CHECK(ml.present);
    CHECK_FALSE(ml.abstain);
    CHECK_FALSE(ml.unavailable);
    CHECK_THAT(ml.p_up, WithinAbs(0.61, 1e-12));
    CHECK_THAT(ml.p_down, WithinAbs(0.39, 1e-12));
    CHECK_THAT(ml.p_target_before_stop, WithinAbs(0.57, 1e-12));
    CHECK_THAT(ml.expected_net_return, WithinAbs(0.0042, 1e-12));
    CHECK_THAT(ml.expected_mfe_long_fraction, WithinAbs(0.0090, 1e-12));
    CHECK_THAT(ml.expected_mae_long_fraction, WithinAbs(0.0031, 1e-12));
    CHECK_THAT(ml.expected_holding_time_min, WithinAbs(95.0, 1e-12));
    CHECK_THAT(ml.conviction, WithinAbs(0.44, 1e-12));
    CHECK(ml.quality_status == "VALID");
    REQUIRE(ml.model_versions.size() == 1);
    CHECK(ml.model_versions[0] == "direction:3");
}

TEST_CASE("the complement of target-before-stop is never invented") {
    // p_target_before_stop = 0.57 does NOT make p_stop_before_target 0.43:
    // a timeout sits between them. AltML publishes no timeout probability, so
    // MlEvaluation has no field for the complement at all.
    const auto ml = ml_from_forecast(good());
    (void)ml;
    STATIC_REQUIRE_FALSE(HasStopBeforeTarget<MlEvaluation>);
    STATIC_REQUIRE_FALSE(HasTimeout<MlEvaluation>);
}
