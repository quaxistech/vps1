// Cost model v2 (§19-§25): the decomposition, and the double-count it replaced.

#include "altstrategy/engine/cost_model.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace altstrategy::engine;
using Catch::Matchers::WithinAbs;

namespace {
DecisionContext ctx_with(double spread_bps, double slippage_bps, bool observed) {
    DecisionContext c;
    c.spread_bps = spread_bps;
    c.expected_slippage_bps = slippage_bps;
    c.spread_observed = observed;
    return c;
}
// The assembler derives expected_slippage as 0.5*spread + depth impact, so a
// context is only realistic if it is built that way.
DecisionContext realistic(double spread_bps, double impact_bps, bool observed = true) {
    return ctx_with(spread_bps, 0.5 * spread_bps + impact_bps, observed);
}
} // namespace

TEST_CASE("a market entry pays taker on both legs and crosses the spread twice") {
    const auto c = compute_trade_cost(EntryType::market, realistic(3.0, 2.0));

    CHECK(c.version == kCostModelVersion);
    CHECK_THAT(c.fee_bps_round_trip, WithinAbs(kTakerFeeBps * 2.0, 1e-9));
    // Two crossings at half the spread each == exactly one spread, not 1.5.
    CHECK_THAT(c.spread_cost_bps, WithinAbs(3.0, 1e-9));
    CHECK_THAT(c.slippage_cost_bps, WithinAbs(2.0, 1e-9));
    CHECK_THAT(c.round_trip_bps, WithinAbs(12.0 + 3.0 + 2.0, 1e-9));
}

TEST_CASE("a limit entry is a maker fill and crosses the spread only on the way out") {
    const auto c = compute_trade_cost(EntryType::limit, realistic(3.0, 2.0));

    CHECK_THAT(c.fee_bps_round_trip, WithinAbs(kMakerFeeBps + kTakerFeeBps, 1e-9));
    CHECK_THAT(c.spread_cost_bps, WithinAbs(1.5, 1e-9));
    CHECK_THAT(c.round_trip_bps, WithinAbs(8.0 + 1.5 + 2.0, 1e-9));

    // A resting entry must be cheaper than crossing for it -- if this ever
    // inverts, the fee table or the leg model is wrong.
    CHECK(c.round_trip_bps < compute_trade_cost(EntryType::market, realistic(3.0, 2.0)).round_trip_bps);
}

TEST_CASE("the spread is never charged one and a half times") {
    // The replaced formula was 2*fees + spread + expected_slippage, and
    // expected_slippage already contained half the spread. At the 10 bps
    // fallback that over-charged 5 bps on every single trade.
    const double spread = 10.0, impact = 2.5;
    const auto ctx = realistic(spread, impact);
    const double old_formula = 2.0 * 4.0 + ctx.spread_bps + ctx.expected_slippage_bps;
    const auto c = compute_trade_cost(EntryType::market, ctx);

    CHECK_THAT(old_formula, WithinAbs(8.0 + 10.0 + 7.5, 1e-9));   // 25.5, as observed live
    CHECK_THAT(c.spread_cost_bps, WithinAbs(spread, 1e-9));        // exactly one spread
    CHECK(c.round_trip_bps < old_formula);
}

TEST_CASE("cost carries whether its spread was measured or assumed") {
    CHECK(compute_trade_cost(EntryType::market, realistic(2.0, 1.0, true)).spread_observed);
    CHECK_FALSE(compute_trade_cost(EntryType::market, realistic(2.0, 1.0, false)).spread_observed);
}

TEST_CASE("a degenerate context cannot produce a negative cost") {
    // Slippage below half the spread would otherwise subtract from the total.
    const auto c = compute_trade_cost(EntryType::market, ctx_with(8.0, 0.0, true));
    CHECK(c.slippage_cost_bps >= 0.0);
    CHECK(c.round_trip_bps >= c.fee_bps_round_trip);

    const auto neg = compute_trade_cost(EntryType::market, ctx_with(-5.0, -5.0, false));
    CHECK(neg.spread_cost_bps >= 0.0);
    CHECK(neg.slippage_cost_bps >= 0.0);
}

TEST_CASE("a stop-market entry is a taker fill, not a maker one") {
    // Live data caught this: stop_market plans were priced at 8.00 bps of fees,
    // the maker+taker figure, because only EntryType::market was treated as
    // crossing. A stop triggers into a market order.
    const auto stop = compute_trade_cost(EntryType::stop_market, realistic(3.0, 2.0));
    const auto mkt  = compute_trade_cost(EntryType::market, realistic(3.0, 2.0));

    CHECK_THAT(stop.fee_bps_round_trip, WithinAbs(kTakerFeeBps * 2.0, 1e-9));
    CHECK_THAT(stop.round_trip_bps, WithinAbs(mkt.round_trip_bps, 1e-9));

    // A stop-LIMIT rests once triggered, so it stays on the maker side.
    CHECK_THAT(compute_trade_cost(EntryType::stop_limit, realistic(3.0, 2.0)).fee_bps_round_trip,
               WithinAbs(kMakerFeeBps + kTakerFeeBps, 1e-9));
}
