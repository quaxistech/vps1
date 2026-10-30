#pragma once

#include "altstrategy/engine/decision_types.hpp"

#include <algorithm>

namespace altstrategy::engine {

// The one definition of what a round trip costs.
//
// Version 3. Unqualified LIMIT/STOP_LIMIT proposals do not prove maker
// execution. Bitget LIMIT defaults to GTC; only explicit timeInForce=post_only
// constrains maker execution. This proposal contract has no such approval, so
// all entry styles retain a conservative taker cost assumption. Fees here are
// a versioned upstream research assumption, never an account FeeQuote/actual fee.
// API: https://www.bitget.com/docs/catalog/trading/order-management
// Version 1 was a flat 8.0 bps struct default in outcomes.hpp that no
// code path overrode -- so every realised outcome was scored at 8 bps while the
// proposal that produced it had been priced at ~25.5, and AltML trained against
// 16. See db/migrations/0005 for why the number is now carried on the row.
//
// Components, each with a source and a formula (nothing here is a round number
// chosen because it looked reasonable):
//
//   fees      Bitget USDT-M perpetuals, per side, from altml.fee_models v1
//             (maker 0.0002, taker 0.0006). Charged per LEG according to how
//             that leg executes; a proposed limit style alone cannot claim the
//             discounted maker rate.
//
//   spread    Crossing the book costs half the quoted spread measured from mid,
//             and only a TAKER pays it -- a resting limit order earns it. The
//             previous formula added the full spread AND expected_slippage_bps,
//             which itself already contained half the spread, charging 1.5x the
//             spread for a round trip that crosses it twice at half each. With
//             the 10 bps fallback that over-charged 5 bps per trade.
//
//   slippage  Depth impact only, with the spread term removed for the reason
//             above. ASSUMPTION (§22): trade size is not a contract parameter
//             here, so this is the venue-rank impact term AltTech's liquidity
//             ranking implies, not a size-aware book walk. When size becomes
//             known this is the term to replace, and the version to bump.
//
// The exit leg is charged as a TAKER. A protective stop is a market order, and
// a take-profit limit that does not fill is not an exit at all -- assuming the
// cheaper leg would flatter every plan that ends at its stop, which is most of
// them.
inline constexpr int kCostModelVersion = 3;
inline constexpr double kMakerFeeBps = 2.0;   // altml.fee_models v1: 0.0002
inline constexpr double kTakerFeeBps = 6.0;   // altml.fee_models v1: 0.0006

struct TradeCost {
    int version = kCostModelVersion;
    double fee_bps_round_trip = 0;
    double spread_cost_bps = 0;
    double slippage_cost_bps = 0;
    double round_trip_bps = 0;
    bool spread_observed = false;
};

// `ctx` supplies the T0 market state. This unqualified proposal's route cannot
// select an actual maker fee or remove the conservative spread reserve.
// Which entry types cross the book. `stop_market` triggers into a market order
// and is as much a taker fill as `market` is -- testing only for `market`
// charged it the maker fee and half the spread, which live data caught. This
// identifies immediate market style only; a false result does not prove maker.
[[nodiscard]] inline constexpr bool crosses_the_book(EntryType t) {
    return t == EntryType::market || t == EntryType::stop_market;
}

[[nodiscard]] inline TradeCost compute_trade_cost(EntryType entry_type, const DecisionContext& ctx) {
    (void)entry_type;

    TradeCost c;
    c.spread_observed = ctx.spread_observed;
    c.fee_bps_round_trip = 2.0 * kTakerFeeBps;

    const double half_spread = std::max(0.0, ctx.spread_bps) / 2.0;
    // Two half-spread reserves, each once. Actual fills/fees stay downstream.
    c.spread_cost_bps = half_spread * 2.0;

    // Recover the depth-impact term the assembler folded into expected_slippage
    // (expected_slippage_bps = 0.5 * spread + depth impact) so the spread is not
    // counted a second time here.
    c.slippage_cost_bps = std::max(0.0, ctx.expected_slippage_bps - half_spread);

    c.round_trip_bps = c.fee_bps_round_trip + c.spread_cost_bps + c.slippage_cost_bps;
    return c;
}

} // namespace altstrategy::engine
