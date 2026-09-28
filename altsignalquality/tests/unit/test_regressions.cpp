// Regression tests for defects found by the institutional-grade audit.
//
// Each one reproduces the defect's mechanism, not merely the symptom: a test
// that only checks the fixed value would pass again the moment the underlying
// mistake returned in a different shape.

#include "altsignalquality/domain/feature_contract.hpp"
#include "altsignalquality/domain/feature_diagnostics.hpp"
#include "altsignalquality/domain/leakage.hpp"
#include "altsignalquality/domain/policy.hpp"
#include "altsignalquality/domain/priors.hpp"
#include "altsignalquality/domain/significance.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>

using namespace altsignalquality::domain;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

SignalSnapshot base_snapshot() {
    SignalSnapshot s;
    s.candidate.side = 1;
    s.candidate.proposal_grade = "NORMAL";
    s.candidate.quality = "STRICT_4_OF_4";
    s.candidate.created_at_ms = 1'770'000'000'000;
    s.candidate.valid_until_ms = s.candidate.created_at_ms + 900'000;
    s.candidate.entry_type = 2;
    s.candidate.holding_class = 2;
    s.candidate.target_distance_bps = 400;
    s.context.present = true;
    s.context.data_quality = "STRICT_4_OF_4";
    s.context.bitget_regime = "TREND_UP";
    s.context.global_regime = "TREND_UP";
    s.context.mark_price = 100.0;
    return s;
}

size_t position_of(std::string_view name) {
    for (size_t i = 0; i < kFeatures.size(); ++i) {
        if (kFeatures[i].name == name) return i;
    }
    return kFeatures.size();
}

} // namespace

// -----------------------------------------------------------------------------
// Defect: an incomplete cost picture was reported as a cheap venue.
//
// `cost_to_target_ratio` summed fees, slippage and spread with value_or(0) and
// only reported MISSING when ALL THREE were absent. A context carrying fees but
// no spread therefore produced a cost that was too LOW — the direction that
// makes a trade look better than it is — and the model could not distinguish
// that number from a genuinely cheap venue.
// -----------------------------------------------------------------------------
TEST_CASE("an incomplete cost picture is MISSING, never a cheaper number") {
    const size_t pos = position_of("cost_to_target_ratio");
    REQUIRE(pos < kFeatureCount);

    auto complete = base_snapshot();
    complete.context.fees_bps = 4.0;
    complete.context.expected_slippage_bps = 3.0;
    complete.context.spread_bps = 2.0;
    const auto full = assemble(complete);
    REQUIRE(full.observed(pos));
    // base_snapshot() is a limit entry: fees 2+6, half-spread 1 on the exit
    // leg, depth impact max(0, 3-1) = 2  ->  11 bps against a 400 bps target.
    // `fees_bps` is set above and deliberately ignored: it is one blended
    // configuration default, and the round trip is priced per leg.
    REQUIRE_THAT(full.values[pos], WithinRel(11.0 / 400.0, 1e-6));

    // Drop any ONE measured component and the feature must go missing, not
    // shrink. Fees are not in this list: they are per-leg constants from the
    // venue's published schedule, not a market observation that can be absent.
    for (int drop = 0; drop < 2; ++drop) {
        auto partial = complete;
        if (drop == 0) partial.context.expected_slippage_bps.reset();
        if (drop == 1) partial.context.spread_bps.reset();
        const auto v = assemble(partial);
        INFO("dropped component " << drop);
        REQUIRE_FALSE(v.observed(pos));
        REQUIRE(std::isnan(v.values[pos]));
    }

    // And an absent blended `fees_bps` no longer removes the feature, because
    // the round trip is no longer priced from it.
    auto no_blend = complete;
    no_blend.context.fees_bps.reset();
    REQUIRE(assemble(no_blend).observed(pos));
}

// -----------------------------------------------------------------------------
// Defect: `strategy_confidence` was a second copy of `adaptive_score`.
//
// AltStrategy assigns both from the same variable (engine/proposal.cpp), so the
// contract carried a duplicate that split the model's feature-sampling budget.
// Verified identical across 115 873 archived proposals before removal.
// -----------------------------------------------------------------------------
TEST_CASE("the contract carries no duplicate of adaptive_score") {
    REQUIRE(position_of("adaptive_score") < kFeatureCount);
    REQUIRE(position_of("strategy_confidence") == kFeatureCount);  // removed
    REQUIRE(std::string(kFeatureSchemaVersion) == "sq-core:6");
    REQUIRE(kFeatureCount == 109);
}

// -----------------------------------------------------------------------------
// Defect: the dataset could not tell a dead feature from a live one.
//
// A third of the declared contract was constant or absent on this stack's real
// history. The diagnostic measured it honestly and reported "constant": 8 every
// cycle -- those eight were fields AltStrategy had stopped producing, arriving
// as protobuf zeros and marked PRESENT by the extractor. Measuring a defect is
// not the same as fixing it: the eight were removed in sq-core:3 and the
// upstream fabrication they came from was removed in AltStrategy itself.
// -----------------------------------------------------------------------------
TEST_CASE("feature diagnostics separate informative, constant, missing and duplicate") {
    const size_t rows = 50;
    std::vector<float> matrix(rows * kFeatureCount, 0.0F);
    std::mt19937 rng(4);
    std::uniform_real_distribution<float> noise(-1.0F, 1.0F);
    for (size_t r = 0; r < rows; ++r) {
        for (size_t f = 0; f < kFeatureCount; ++f) {
            matrix[r * kFeatureCount + f] = noise(rng);
        }
        matrix[r * kFeatureCount + 0] = 0.25F;                              // constant
        matrix[r * kFeatureCount + 1] = std::numeric_limits<float>::quiet_NaN();  // absent
        matrix[r * kFeatureCount + 3] = matrix[r * kFeatureCount + 2];      // duplicate
    }
    const auto d = diagnose_features(matrix, rows);
    REQUIRE(d.stats.size() == kFeatureCount);
    REQUIRE(d.stats[0].constant);
    REQUIRE_THAT(d.stats[0].min_value, WithinAbs(0.25, 1e-6));
    REQUIRE(d.stats[1].all_missing);
    REQUIRE(d.stats[3].duplicate_of == std::string(kFeatures[2].name));
    REQUIRE(d.constant >= 1);
    REQUIRE(d.all_missing >= 1);
    REQUIRE(d.duplicates >= 1);
    REQUIRE(d.informative == static_cast<int64_t>(kFeatureCount) - d.constant -
                                 d.all_missing - d.duplicates);
    REQUIRE(d.informative_share() < 1.0);
}

TEST_CASE("feature diagnostics reject a matrix that does not match the contract") {
    REQUIRE(diagnose_features({}, 0).stats.empty());
    REQUIRE(diagnose_features(std::vector<float>(kFeatureCount + 1, 0.0F), 1).stats.empty());
}

// -----------------------------------------------------------------------------
// Defect: the as-of prior sweep was not deterministic.
//
// Outcomes were ordered by (matured_at, strategy_id), which is not a total
// order — two outcomes of one strategy maturing in the same millisecond compare
// equal, std::sort is not stable, and the decayed statistics then depended on
// the library's partitioning. The same dataset could produce a different model.
// -----------------------------------------------------------------------------
TEST_CASE("the as-of sweep is deterministic under ties") {
    std::vector<MaturedOutcome> outcomes;
    for (int i = 0; i < 200; ++i) {
        MaturedOutcome o;
        o.matured_at_ms = 1'000'000 + (i / 20) * 86'400'000;  // 20 outcomes share an instant
        o.signal_proposal_id = "sig-" + std::to_string(i);
        o.strategy_id = "alpha";                              // and a strategy
        o.canonical_pair_id = 7;
        o.regime = "TREND_UP";
        o.grade = "NORMAL";
        o.side = 1;
        o.label = i % 3 == 0 ? 1 : 0;
        o.net_return = i % 3 == 0 ? 0.02 : -0.01;
        outcomes.push_back(o);
    }
    PriorReadRequest read;
    read.observation_time_ms = 1'000'000 + 10 * 86'400'000;
    read.strategy_id = "alpha";
    read.canonical_pair_id = 7;
    read.regime = "TREND_UP";
    read.grade = "NORMAL";
    read.side = 1;

    const auto reference = sweep_as_of(outcomes, {read});
    std::mt19937 rng(11);
    for (int trial = 0; trial < 8; ++trial) {
        auto shuffled = outcomes;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        const auto got = sweep_as_of(shuffled, {read});
        INFO("trial " << trial);
        REQUIRE_THAT(got[0].strategy.win_rate,
                     WithinAbs(reference[0].strategy.win_rate, 1e-15));
        REQUIRE_THAT(got[0].strategy.expectancy,
                     WithinAbs(reference[0].strategy.expectancy, 1e-15));
        REQUIRE_THAT(got[0].decayed_strategy.effective_samples,
                     WithinAbs(reference[0].decayed_strategy.effective_samples, 1e-12));
        REQUIRE_THAT(got[0].decayed_strategy.win_rate,
                     WithinAbs(reference[0].decayed_strategy.win_rate, 1e-12));
    }
}

// -----------------------------------------------------------------------------
// Defect: a stale signal was reported as an unusable model.
// -----------------------------------------------------------------------------
TEST_CASE("staleness is reported as staleness, not as a broken model") {
    DecisionConfig cfg;
    const auto d = decide(cfg, true, false, 0, 0, /*model_unusable=*/false, /*stale=*/true);
    REQUIRE(d.basis == DecisionBasis::signal_stale);
    REQUIRE_FALSE(d.accepted);
    REQUIRE_FALSE(d.has_score);
}

// -----------------------------------------------------------------------------
// New requirement: a measured edge must come with an interval, and the interval
// must respect the dependence between overlapping signals.
// -----------------------------------------------------------------------------
TEST_CASE("the block bootstrap brackets a known mean") {
    std::vector<double> values(400);
    std::mt19937 rng(7);
    std::normal_distribution<double> normal(0.01, 0.02);
    for (auto& v : values) v = normal(rng);
    const auto ci = block_bootstrap_mean(values);
    REQUIRE(ci.samples == 400);
    REQUIRE(ci.block_length >= 2);
    REQUIRE(ci.lower < ci.point);
    REQUIRE(ci.upper > ci.point);
    REQUIRE(ci.lower < 0.01);
    REQUIRE(ci.upper > 0.01);
}

TEST_CASE("the block bootstrap is wider than an IID interval on dependent data") {
    // A strongly autocorrelated series: an IID interval would badly understate
    // the uncertainty, which is exactly the failure mode for overlapping trades.
    std::vector<double> dependent(600);
    std::mt19937 rng(3);
    std::normal_distribution<double> shock(0.0, 0.01);
    double level = 0.0;
    for (auto& v : dependent) {
        level = 0.98 * level + shock(rng);
        v = level;
    }
    const auto blocked = block_bootstrap_mean(dependent, 0.95, 2000, 1U);
    // The same routine with block length 1 IS the IID bootstrap; approximate it
    // by bootstrapping a shuffled copy, which destroys the dependence.
    auto shuffled = dependent;
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    const auto iid = block_bootstrap_mean(shuffled, 0.95, 2000, 1U);
    REQUIRE((blocked.upper - blocked.lower) > (iid.upper - iid.lower));
}

TEST_CASE("a sample too small for an interval reports the point estimate only") {
    const auto ci = block_bootstrap_mean({0.01, 0.02, 0.03});
    REQUIRE_THAT(ci.point, WithinRel(0.02, 1e-9));
    REQUIRE(std::isnan(ci.lower));
    REQUIRE(std::isnan(ci.upper));
    REQUIRE(block_bootstrap_mean({}).samples == 0);
}

TEST_CASE("the uplift interval straddles zero when the filter has no edge") {
    // Acceptance uncorrelated with outcome: the true uplift is zero.
    std::vector<double> net(500);
    std::vector<char> accepted(500);
    std::mt19937 rng(5);
    std::normal_distribution<double> normal(0.0, 0.02);
    for (size_t i = 0; i < net.size(); ++i) {
        net[i] = normal(rng);
        accepted[i] = (i % 2 == 0) ? 1 : 0;
    }
    const auto ci = block_bootstrap_uplift(net, accepted);
    REQUIRE(ci.lower < 0.0);
    REQUIRE(ci.upper > 0.0);
    REQUIRE_FALSE(ci.excludes_zero_below());
}

TEST_CASE("the uplift interval clears zero when the filter genuinely selects winners") {
    std::vector<double> net(500);
    std::vector<char> accepted(500);
    std::mt19937 rng(9);
    std::normal_distribution<double> normal(0.0, 0.005);
    for (size_t i = 0; i < net.size(); ++i) {
        const bool good = i % 2 == 0;
        net[i] = (good ? 0.03 : -0.03) + normal(rng);
        accepted[i] = good ? 1 : 0;
    }
    const auto ci = block_bootstrap_uplift(net, accepted);
    REQUIRE(ci.excludes_zero_below());
    REQUIRE(ci.point > 0.0);
}

TEST_CASE("the Wilson interval stays inside [0,1] where the normal one would not") {
    const auto perfect = wilson_interval(12, 12);
    REQUIRE_THAT(perfect.point, WithinAbs(1.0, 1e-12));
    REQUIRE(perfect.upper <= 1.0);
    REQUIRE(perfect.lower < 1.0);
    REQUIRE(perfect.lower > 0.5);

    const auto none = wilson_interval(0, 20);
    REQUIRE_THAT(none.point, WithinAbs(0.0, 1e-12));
    REQUIRE(none.lower >= 0.0);
    REQUIRE(none.upper > 0.0);

    REQUIRE(wilson_interval(0, 0).samples == 0);
}

// -----------------------------------------------------------------------------
// Defect: a single malformed upstream row would have stopped the dataset
// growing forever.
//
// `insert_training_examples` wrote a whole batch in one transaction. A row
// violating a CHECK — a label that matured at or before its own signal, say —
// aborted the transaction, and the query that produced the batch returned the
// SAME rows on the next pass. The invariants are therefore checked in the
// domain first, and the offending row is rejected rather than the batch.
//
// The check itself lives in the store (it needs the row type), so what is
// pinned here is the invariant it enforces, against the same rule the leakage
// audit applies to a dataset.
// -----------------------------------------------------------------------------
TEST_CASE("a label that matured at or before its signal is never a usable example") {
    std::vector<RowTimes> rows = {{1'000, 2'000}, {2'000, 2'000}, {3'000, 2'999}};
    const std::vector<std::string> ids = {"ok", "equal", "before"};
    const auto audit = audit_rows(rows, ids);
    REQUIRE_FALSE(audit.clean());
    // Both the equal and the earlier maturation must be reported; only the
    // first row is admissible.
    int reported = 0;
    for (const auto& f : audit.findings) {
        if (f.find("matured at or before") != std::string::npos) ++reported;
    }
    REQUIRE(reported == 2);
}

// -----------------------------------------------------------------------------
// Defect: an unselected threshold defaulted to 0.5.
//
// A caller that forgot to check `found` would have accepted roughly half the
// signals it was shown. The default is now NaN, which fails every comparison.
// -----------------------------------------------------------------------------
TEST_CASE("an unfound threshold cannot be used by accident") {
    ThresholdSearchConfig cfg;
    const auto none = choose_threshold({}, {}, {}, cfg);
    REQUIRE_FALSE(none.found);
    REQUIRE(std::isnan(none.threshold));
    // The comparison a careless caller would make accepts nothing.
    REQUIRE_FALSE(0.99 >= none.threshold);
    REQUIRE_FALSE(0.01 >= none.threshold);
}

// -----------------------------------------------------------------------------
// The gate that decides promotion reads the LOWER bound. This pins the
// behaviour that makes it meaningful: a filter with a real edge clears it, and
// the same filter measured on too few trades does not.
// -----------------------------------------------------------------------------
TEST_CASE("a real edge measured on too few trades does not clear the bound") {
    const auto make = [](size_t n) {
        std::vector<double> net(n);
        std::vector<char> accepted(n);
        std::mt19937 rng(21);
        std::normal_distribution<double> noise(0.0, 0.04);
        for (size_t i = 0; i < n; ++i) {
            const bool good = i % 2 == 0;
            net[i] = (good ? 0.004 : -0.004) + noise(rng);
            accepted[i] = good ? 1 : 0;
        }
        return block_bootstrap_uplift(net, accepted);
    };
    const auto small = make(30);
    const auto large = make(3000);
    // Same generating process, same true edge; only the evidence differs.
    REQUIRE(large.point > 0.0);
    REQUIRE((large.upper - large.lower) < (small.upper - small.lower));
    REQUIRE(large.excludes_zero_below());
    REQUIRE_FALSE(small.excludes_zero_below());
}

// -----------------------------------------------------------------------------
// The duplicate search is guarded by a cheap signature so it does not become
// O(rows x features^2) inside a training cycle. The signature can collide; the
// full column comparison is what decides, and these pin both directions.
// -----------------------------------------------------------------------------
TEST_CASE("the duplicate search finds real duplicates and invents none") {
    const size_t rows = 200;
    std::vector<float> matrix(rows * kFeatureCount);
    std::mt19937 rng(31);
    std::uniform_real_distribution<float> noise(-1.0F, 1.0F);
    for (size_t r = 0; r < rows; ++r) {
        for (size_t f = 0; f < kFeatureCount; ++f) {
            matrix[r * kFeatureCount + f] = noise(rng);
        }
        // A true duplicate.
        matrix[r * kFeatureCount + 5] = matrix[r * kFeatureCount + 4];
        // A near-miss: identical summary statistics (same values, permuted
        // across two rows) but a different column. It must NOT be reported.
        matrix[r * kFeatureCount + 7] = (r % 2 == 0) ? 0.25F : 0.75F;
        matrix[r * kFeatureCount + 8] = (r % 2 == 0) ? 0.75F : 0.25F;
        // Identical missing patterns must not be enough on their own either.
        if (r % 3 == 0) {
            matrix[r * kFeatureCount + 9] = std::numeric_limits<float>::quiet_NaN();
            matrix[r * kFeatureCount + 10] = std::numeric_limits<float>::quiet_NaN();
        }
    }
    const auto d = diagnose_features(matrix, rows);
    REQUIRE(d.stats[5].duplicate_of == std::string(kFeatures[4].name));
    REQUIRE(d.stats[7].duplicate_of.empty());
    // Two constants holding the same value are trivially identical; reporting
    // them as duplicates of each other buries the finding that matters. On this
    // stack's real data that was nine features all "duplicating" one another
    // because they all happened to equal 1.0.
    {
        std::vector<float> constants(rows * kFeatureCount, 0.5F);
        for (size_t r = 0; r < rows; ++r) {
            constants[r * kFeatureCount + 20] = 1.0F;
            constants[r * kFeatureCount + 21] = 1.0F;
        }
        const auto c = diagnose_features(constants, rows);
        REQUIRE(c.stats[20].constant);
        REQUIRE(c.stats[21].constant);
        REQUIRE(c.stats[20].duplicate_of.empty());
        REQUIRE(c.stats[21].duplicate_of.empty());
    }
    REQUIRE(d.stats[8].duplicate_of.empty());
    REQUIRE(d.stats[9].duplicate_of.empty());
    REQUIRE(d.stats[10].duplicate_of.empty());
    // Position 4 is the survivor; only the later column is flagged.
    REQUIRE(d.stats[4].duplicate_of.empty());
}

// -----------------------------------------------------------------------------
// Defect: the diagnostic's own variance could report zero for a column that
// plainly varies.
//
// `sum_sq/n - mean^2` cancels catastrophically at the magnitudes some of these
// features actually carry — top-of-book depth reaches 1.1e8 USDT in this
// stack's archive — so the check meant to detect a dead feature could declare a
// live one dead. Welford's online variance is used instead.
// -----------------------------------------------------------------------------
TEST_CASE("feature variance survives large magnitudes") {
    const size_t rows = 64;
    std::vector<float> matrix(rows * kFeatureCount, 1.0F);
    // A column at the scale of real order-book depth, varying by a few parts in
    // ten thousand — small relative to the magnitude, but unmistakably not
    // constant. The naive formula loses this difference entirely in float64.
    for (size_t r = 0; r < rows; ++r) {
        matrix[r * kFeatureCount + 0] = 1.0e8F + static_cast<float>(r) * 1.0e4F;
    }
    const auto d = diagnose_features(matrix, rows);
    REQUIRE_FALSE(d.stats[0].constant);
    REQUIRE(d.stats[0].stddev > 0.0);
    REQUIRE(d.stats[0].max_value > d.stats[0].min_value);
    // And the mean is still the mean, not a cancelled remnant.
    REQUIRE(d.stats[0].mean > 1.0e8);
    REQUIRE(d.stats[0].mean < 1.1e8);
}

// -----------------------------------------------------------------------------
// Defect: a zero mark price was used as a real price.
//
// `(mark - ema200) / ema200` returns a perfectly finite −10 000 bps for
// mark = 0, so every downstream guard passed and the model would have learned
// from a price that never existed. A zero or negative mark is not a price; it
// is a broken row, and every feature that references it must go MISSING.
// -----------------------------------------------------------------------------
TEST_CASE("an impossible mark price is rejected, not computed with") {
    auto s = base_snapshot();
    s.context.bitget_features = {{"ema_200", 96.0}, {"atr_14", 1.5}, {"stddev_20", 1.1},
                                 {"donchian_high_20", 105.0}, {"donchian_low_20", 95.0}};
    s.candidate.entry_price = 100.0;

    const auto sound = assemble(s);
    REQUIRE(sound.observed(position_of("price_vs_ema200_bps")));
    REQUIRE(sound.observed(position_of("entry_offset_bps")));
    REQUIRE(sound.observed(position_of("atr_14_bps")));

    for (const double impossible : {0.0, -1.0, -100.5}) {
        auto broken = s;
        broken.context.mark_price = impossible;
        const auto v = assemble(broken);
        INFO("mark price " << impossible);
        for (const char* name : {"price_vs_ema200_bps", "entry_offset_bps", "atr_14_bps",
                                 "stddev_20_bps", "donchian_position_20"}) {
            INFO(name);
            REQUIRE_FALSE(v.observed(position_of(name)));
        }
    }

    // An impossible ENTRY price is equally not a price.
    auto no_entry = s;
    no_entry.candidate.entry_price = 0.0;
    REQUIRE_FALSE(assemble(no_entry).observed(position_of("entry_offset_bps")));
}

// -----------------------------------------------------------------------------
// §44: the 48h ceiling is enforced HERE, not trusted from AltStrategy.
//
// AltSignalQuality is the last stage that can refuse a signal before delivery.
// An invariant enforced in exactly one place upstream is enforced only for as
// long as that place stays correct, and this stage had no check of its own.
// -----------------------------------------------------------------------------
TEST_CASE("a signal holding past the 48h ceiling is refused independently") {
    DecisionConfig cfg;
    cfg.require_upstream_recommendation = false;

    // One millisecond over, with everything else about the signal ideal:
    // recommended upstream, a usable model, and a score far above threshold.
    const auto over = decide(cfg, /*upstream_recommended=*/true, /*scored=*/true,
                             /*quality_score=*/0.99, /*threshold=*/0.5,
                             /*model_unusable=*/false, /*stale=*/false,
                             kMaxHoldingMs + 1);
    CHECK_FALSE(over.accepted);
    CHECK(over.basis == DecisionBasis::holding_exceeds_ceiling);
    CHECK_FALSE(over.has_score);          // no score is reported for a refused signal
    REQUIRE_FALSE(over.reason_codes.empty());
    CHECK(over.reason_codes[0] == "HOLDING_EXCEEDS_CEILING");

    // Exactly at the ceiling is allowed: the bound is inclusive.
    const auto at = decide(cfg, true, true, 0.99, 0.5, false, false, kMaxHoldingMs);
    CHECK(at.accepted);
    CHECK(at.basis == DecisionBasis::cold_start);

    // The refusal outranks staleness, so an operator sees the invariant breach
    // rather than a freshness complaint that would send them after the wrong
    // thing.
    const auto both = decide(cfg, true, true, 0.99, 0.5, false, /*stale=*/true,
                             kMaxHoldingMs + 1);
    CHECK(both.basis == DecisionBasis::holding_exceeds_ceiling);

    // And it round-trips through the persisted name.
    CHECK(parse_decision_basis(decision_basis_name(DecisionBasis::holding_exceeds_ceiling))
          == DecisionBasis::holding_exceeds_ceiling);
}

// -----------------------------------------------------------------------------
// Defect: two derived economics reported 0.0 for an estimate nobody made.
//
// The raw block guards `p_target_before_stop` and `expected_net_return` on
// `edge_estimated` and writes them ABSENT when AltStrategy had no model to
// produce them — with a comment saying that some(0.0) would state a measurement
// instead of the truth. The two features derived from those same numbers,
// forty lines further down, were written unconditionally: `net_edge_bps` came
// out as exactly 0.0 bps and `rr_times_p_target` as exactly 0.0, on every
// signal. Measured constant across all 3752 archived examples, because
// `edge_estimated` is false for as long as AltML has no active model.
// -----------------------------------------------------------------------------
TEST_CASE("economics derived from an absent estimate are absent, not zero") {
    const size_t net_edge = position_of("net_edge_bps");
    const size_t rr_p = position_of("rr_times_p_target");
    REQUIRE(net_edge < kFeatureCount);
    REQUIRE(rr_p < kFeatureCount);

    auto absent = base_snapshot();
    absent.candidate.edge_estimated = false;
    absent.candidate.expected_net_return = 0.0;
    absent.candidate.p_target_before_stop = std::nullopt;
    absent.candidate.expected_rr = 1.5;
    const auto no_estimate = assemble(absent);
    REQUIRE_FALSE(no_estimate.observed(net_edge));
    REQUIRE_FALSE(no_estimate.observed(rr_p));
    // The raw members they derive from are absent for the same reason.
    REQUIRE_FALSE(no_estimate.observed(position_of("expected_net_return")));
    REQUIRE_FALSE(no_estimate.observed(position_of("p_target_before_stop")));

    // With an estimate present they carry it, in the units the names claim.
    auto estimated = absent;
    estimated.candidate.edge_estimated = true;
    estimated.candidate.expected_net_return = 0.0125;  // 125 bps
    estimated.candidate.p_target_before_stop = 0.4;
    const auto with_estimate = assemble(estimated);
    REQUIRE(with_estimate.observed(net_edge));
    REQUIRE(with_estimate.observed(rr_p));
    REQUIRE_THAT(with_estimate.values[net_edge], WithinRel(125.0, 1e-6));
    REQUIRE_THAT(with_estimate.values[rr_p], WithinRel(0.6, 1e-6));
}


// -----------------------------------------------------------------------------
// Defect: this service priced a round trip with a formula AltStrategy had
// already retired.
//
// `cost_to_target_ratio` computed `2*fees_bps + expected_slippage_bps +
// spread_bps`. AltStrategy's cost model documents why that is wrong:
// `expected_slippage_bps` already carries half the spread, so adding the whole
// spread again charges 1.5x of it; and `ctx.fees_bps` is one blended config
// default, so the number did not move with the entry type. Measured on six
// hours of live context: the feature reported 14.7 bps for every signal while
// the producer's own round trip averaged 15.1 and ranged 11.9 (resting limit)
// to 17.3 (market).
// -----------------------------------------------------------------------------
TEST_CASE("the cost feature prices a round trip the way AltStrategy does",
          "[cost][regression]") {
    const size_t pos = position_of("cost_to_target_ratio");
    REQUIRE(pos < kFeatureCount);

    auto s = base_snapshot();
    s.context.fees_bps = 4.0;               // the blended default; must be ignored
    s.context.spread_bps = 3.0;
    s.context.expected_slippage_bps = 5.0;  // 1.5 of it is the half-spread
    s.candidate.target_distance_bps = 400;

    SECTION("a market entry crosses the book on both legs") {
        s.candidate.entry_type = 1;  // ENTRY_MARKET
        // fees 6+6, spread 1.5*2, slippage max(0, 5-1.5) = 3.5  ->  18.5 bps
        REQUIRE_THAT(assemble(s).values[pos], WithinRel(18.5 / 400.0, 1e-6));
    }
    SECTION("a resting limit entry pays the spread once") {
        s.candidate.entry_type = 2;  // ENTRY_LIMIT
        // fees 2+6, spread 1.5, slippage 3.5  ->  13.0 bps
        REQUIRE_THAT(assemble(s).values[pos], WithinRel(13.0 / 400.0, 1e-6));
    }
    SECTION("a stop-market entry is a taker, a stop-limit is not") {
        s.candidate.entry_type = 3;  // ENTRY_STOP_MARKET
        const double taker = assemble(s).values[pos];
        s.candidate.entry_type = 4;  // ENTRY_STOP_LIMIT
        const double maker = assemble(s).values[pos];
        REQUIRE(taker > maker);
        REQUIRE_THAT(taker, WithinRel(18.5 / 400.0, 1e-6));
        REQUIRE_THAT(maker, WithinRel(13.0 / 400.0, 1e-6));
    }
    SECTION("the spread is never charged one and a half times") {
        // The retired formula would have produced 2*4 + 5 + 3 = 16 bps here,
        // independent of the entry type.
        s.candidate.entry_type = 2;
        REQUIRE(std::abs(assemble(s).values[pos] * 400.0 - 16.0) > 1e-6);
    }
    SECTION("an incomplete cost picture is still MISSING, never cheaper") {
        s.candidate.entry_type = 1;
        auto partial = s;
        partial.context.spread_bps.reset();
        REQUIRE_FALSE(assemble(partial).observed(pos));
    }
}

// -----------------------------------------------------------------------------
// ML-first: an econ model estimates the edge but NOT a target-before-stop
// probability. edge_estimated = true must not turn the absent probability into
// a measured 0.0 -- nor the RR-weighted figure derived from it.
// -----------------------------------------------------------------------------
TEST_CASE("an estimated edge without a target probability leaves that probability absent") {
    auto s = base_snapshot();
    s.candidate.edge_estimated = true;
    s.candidate.expected_net_return = 0.0008;          // +8 bps, from the econ model
    s.candidate.p_target_before_stop = std::nullopt;   // not produced
    s.candidate.expected_rr = 1.9;
    const auto v = assemble(s);
    REQUIRE(v.observed(position_of("expected_net_return")));
    REQUIRE(v.observed(position_of("net_edge_bps")));
    REQUIRE_FALSE(v.observed(position_of("p_target_before_stop")));
    REQUIRE_FALSE(v.observed(position_of("rr_times_p_target")));
}
