// The as-of priors are the one feature block where leakage is easy, invisible
// and fatal: a strategy's historical win rate read from today's database would
// hand a signal from June knowledge of August. These tests pin the sweep's
// time semantics, the shrinkage and the decay.

#include "altsignalquality/domain/priors.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace altsignalquality::domain;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr int64_t kDay = 86'400'000;

MaturedOutcome outcome(int64_t matured_ms, std::string strategy, int label, double net) {
    MaturedOutcome o;
    o.matured_at_ms = matured_ms;
    o.strategy_id = std::move(strategy);
    o.canonical_pair_id = 7;
    o.regime = "TREND_UP";
    o.grade = "NORMAL";
    o.side = 1;
    o.label = label;
    o.net_return = net;
    return o;
}

PriorReadRequest read_at(int64_t t, std::string strategy = "alpha") {
    PriorReadRequest r;
    r.observation_time_ms = t;
    r.strategy_id = std::move(strategy);
    r.canonical_pair_id = 7;
    r.regime = "TREND_UP";
    r.grade = "NORMAL";
    r.side = 1;
    return r;
}

} // namespace

TEST_CASE("a read before any outcome matured sees nothing at all") {
    const std::vector<MaturedOutcome> history = {outcome(10 * kDay, "alpha", 1, 0.01)};
    const auto snaps = sweep_as_of(history, {read_at(5 * kDay)});
    REQUIRE(snaps.size() == 1);
    REQUIRE_FALSE(snaps[0].global.present);
    REQUIRE_FALSE(snaps[0].strategy.present);
}

TEST_CASE("a read only sees outcomes that had already matured") {
    std::vector<MaturedOutcome> history;
    for (int i = 1; i <= 10; ++i) {
        history.push_back(outcome(static_cast<int64_t>(i) * kDay, "alpha", i <= 5 ? 1 : 0, 0.01));
    }
    // At day 5.5 exactly five outcomes had matured, and all five were wins.
    const auto snaps = sweep_as_of(history, {read_at(5 * kDay + kDay / 2)});
    REQUIRE(snaps[0].global.present);
    REQUIRE_THAT(snaps[0].global.effective_samples, WithinAbs(5.0, 1e-9));
    REQUIRE_THAT(snaps[0].global.win_rate, WithinAbs(1.0, 1e-9));
}

TEST_CASE("an outcome that matured exactly at T0 is knowable at T0") {
    const std::vector<MaturedOutcome> history = {outcome(3 * kDay, "alpha", 1, 0.01)};
    const auto snaps = sweep_as_of(history, {read_at(3 * kDay)});
    REQUIRE(snaps[0].global.present);
    REQUIRE_THAT(snaps[0].global.effective_samples, WithinAbs(1.0, 1e-9));
}

TEST_CASE("the sweep is independent of the order the reads are given in") {
    std::vector<MaturedOutcome> history;
    for (int i = 1; i <= 20; ++i) {
        history.push_back(outcome(static_cast<int64_t>(i) * kDay, "alpha", i % 3 == 0 ? 1 : 0,
                                  i % 3 == 0 ? 0.02 : -0.01));
    }
    const std::vector<PriorReadRequest> forward = {read_at(4 * kDay), read_at(11 * kDay),
                                                   read_at(19 * kDay)};
    const std::vector<PriorReadRequest> shuffled = {read_at(19 * kDay), read_at(4 * kDay),
                                                    read_at(11 * kDay)};
    const auto a = sweep_as_of(history, forward);
    const auto b = sweep_as_of(history, shuffled);
    // b[1] is the day-4 read, b[2] the day-11 read, b[0] the day-19 read.
    REQUIRE_THAT(a[0].global.effective_samples, WithinAbs(b[1].global.effective_samples, 1e-9));
    REQUIRE_THAT(a[1].global.effective_samples, WithinAbs(b[2].global.effective_samples, 1e-9));
    REQUIRE_THAT(a[2].global.effective_samples, WithinAbs(b[0].global.effective_samples, 1e-9));
    REQUIRE_THAT(a[1].strategy.win_rate, WithinAbs(b[2].strategy.win_rate, 1e-12));
}

TEST_CASE("an unseen cell reports the global picture with zero own evidence") {
    std::vector<MaturedOutcome> history;
    for (int i = 1; i <= 40; ++i) {
        history.push_back(outcome(static_cast<int64_t>(i) * kDay, "alpha", i % 2, 0.01));
    }
    const auto snaps = sweep_as_of(history, {read_at(50 * kDay, "brand_new_strategy")});
    REQUIRE(snaps[0].strategy.present);
    REQUIRE_THAT(snaps[0].strategy.effective_samples, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(snaps[0].strategy.win_rate, WithinAbs(snaps[0].global.win_rate, 1e-12));
}

TEST_CASE("a small cell is shrunk toward the global estimate") {
    // 40 outcomes globally, half wins; `beta` has 4 outcomes, all wins.
    std::vector<MaturedOutcome> history;
    for (int i = 1; i <= 40; ++i) {
        history.push_back(outcome(static_cast<int64_t>(i) * kDay, "alpha", i % 2, 0.0));
    }
    for (int i = 1; i <= 4; ++i) {
        history.push_back(outcome(static_cast<int64_t>(i) * kDay, "beta", 1, 0.0));
    }
    PriorConfig cfg;
    cfg.prior_strength = 20.0;
    const auto snaps = sweep_as_of(history, {read_at(60 * kDay, "beta")}, cfg);
    const double global_rate = snaps[0].global.win_rate;
    // w = 4 / (4 + 20) = 1/6, so the reported rate is 1/6*1.0 + 5/6*global.
    const double expected = (4.0 / 24.0) * 1.0 + (20.0 / 24.0) * global_rate;
    REQUIRE_THAT(snaps[0].strategy.win_rate, WithinRel(expected, 1e-9));
    REQUIRE(snaps[0].strategy.win_rate < 1.0);
    REQUIRE_THAT(snaps[0].strategy.effective_samples, WithinAbs(4.0, 1e-9));
}

TEST_CASE("a cell at the pseudo-count weights its own evidence exactly half") {
    std::vector<MaturedOutcome> history;
    for (int i = 1; i <= 100; ++i) {          // global: all losses
        history.push_back(outcome(static_cast<int64_t>(i) * kDay, "alpha", 0, 0.0));
    }
    for (int i = 1; i <= 20; ++i) {           // gamma: 20 wins, exactly k
        history.push_back(outcome(static_cast<int64_t>(i) * kDay, "gamma", 1, 0.0));
    }
    PriorConfig cfg;
    cfg.prior_strength = 20.0;
    const auto snaps = sweep_as_of(history, {read_at(200 * kDay, "gamma")}, cfg);
    const double global_rate = snaps[0].global.win_rate;
    REQUIRE_THAT(snaps[0].strategy.win_rate, WithinRel(0.5 * 1.0 + 0.5 * global_rate, 1e-9));
}

TEST_CASE("the decayed view forgets old evidence while the plain view does not") {
    std::vector<MaturedOutcome> history;
    for (int i = 1; i <= 30; ++i) {           // ancient wins
        history.push_back(outcome(static_cast<int64_t>(i) * kDay, "alpha", 1, 0.02));
    }
    PriorConfig cfg;
    cfg.half_life_days = 30.0;
    cfg.prior_strength = 20.0;
    const auto soon = sweep_as_of(history, {read_at(31 * kDay)}, cfg);
    const auto much_later = sweep_as_of(history, {read_at(400 * kDay)}, cfg);

    // The undecayed sample count is unchanged by the passage of time...
    REQUIRE_THAT(soon[0].strategy.effective_samples,
                 WithinAbs(much_later[0].strategy.effective_samples, 1e-9));
    // ...while the decayed weight collapses, so the decayed cell falls back
    // toward the global estimate instead of asserting a decade-old edge.
    REQUIRE(much_later[0].decayed_strategy.effective_samples <
            soon[0].decayed_strategy.effective_samples * 0.05);
}

TEST_CASE("an accumulator restored from persisted counters continues correctly") {
    PriorAccumulator live;
    for (int i = 1; i <= 10; ++i) {
        live.apply(outcome(static_cast<int64_t>(i) * kDay, "alpha", 1, 0.01));
    }
    PriorAccumulator restored;
    for (const auto& [key, counters] : live.cells()) {
        const auto [kind, scope_key] = PriorAccumulator::split_key(key);
        const PriorScope scope = kind == "GLOBAL"            ? PriorScope::global
                                 : kind == "STRATEGY"        ? PriorScope::strategy
                                 : kind == "PAIR"            ? PriorScope::pair
                                 : kind == "REGIME"          ? PriorScope::regime
                                 : kind == "STRATEGY_REGIME" ? PriorScope::strategy_regime
                                 : kind == "STRATEGY_SIDE"   ? PriorScope::strategy_side
                                                             : PriorScope::grade;
        restored.load_cell(scope, scope_key, counters);
    }
    const auto a = live.snapshot(20 * kDay, "alpha", 7, "TREND_UP", "NORMAL", 1);
    const auto b = restored.snapshot(20 * kDay, "alpha", 7, "TREND_UP", "NORMAL", 1);
    REQUIRE_THAT(a.strategy.win_rate, WithinAbs(b.strategy.win_rate, 1e-12));
    REQUIRE_THAT(a.strategy.expectancy, WithinAbs(b.strategy.expectancy, 1e-12));
    REQUIRE_THAT(a.decayed_strategy.effective_samples,
                 WithinAbs(b.decayed_strategy.effective_samples, 1e-9));
}

TEST_CASE("the scope key round-trips through its persisted form") {
    const auto o = outcome(kDay, "alpha", 1, 0.0);
    for (const auto& [scope, key] : PriorAccumulator::scopes_of(o)) {
        const auto encoded = PriorAccumulator::cell_key(scope, key);
        const auto [kind, decoded] = PriorAccumulator::split_key(encoded);
        INFO(encoded);
        REQUIRE(kind == std::string(prior_scope_name(scope)));
        REQUIRE(decoded == key);
    }
}
