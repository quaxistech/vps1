#pragma once

// trade_plan_generator (§11, §14, §15). For a candidate it builds three immutable
// geometries — AGGRESSIVE / BALANCED / CONSERVATIVE — each with an entry type +
// price/zone, a fill-time estimate + entry timeout, a structure/ATR/ML-derived
// stop, a structure/ML-derived target, and a time-stop capped at 48h. Prices are
// tick-rounded and the LONG/SHORT price-side invariants are preserved.

#include "altstrategy/engine/decision_types.hpp"

#include <vector>

namespace altstrategy::engine {

// Fill-time estimate (§26 first prompt / §12). Minutes, plus a bounded timeout.
struct FillEstimate {
    double p50_min = 0;
    double p90_min = 0;
    int64_t timeout_ms = 0;
};

// Estimate fill time for an entry a given distance (in bps) from the market,
// using spread, top-of-book depth, trade intensity, volatility and liquidity.
[[nodiscard]] FillEstimate estimate_fill(EntryType type, double distance_bps, double spread_bps,
                                         double top_depth_usdt, double trade_intensity,
                                         double volatility_bps, double liquidity_score);

struct PlanConfig {
    double tick_size = 0;            // 0 => derive from price magnitude
    double min_stop_bps = 8.0;      // never tighter than this
    int64_t scalp_max_ms = 30LL * 60 * 1000;
    int64_t intraday_max_ms = 8LL * 3600 * 1000;
    // The EXTENDED_INTRADAY class cap. May be configured DOWN, never up: the
    // absolute ceiling is kMaxHoldingMs and application.cpp refuses a larger
    // value at startup, while generate_plans() bounds by it regardless.
    int64_t extended_max_ms = kMaxHoldingMs;
    int64_t max_entry_timeout_ms = 2LL * 3600 * 1000; // entry can't wait 48h (§14)
    double default_max_slippage_bps = 6.0;
};

// Round a price to the tick grid (derived from magnitude when tick unknown).
// Which entry a candidate would be placed with. Exposed because the RANKING
// has to price the same fill the plan will actually use: a limit entry is a
// maker fill and costs a third of a market entry's fee, and ranking a candidate
// against the wrong leg is ranking it against a cost it will never pay.
[[nodiscard]] EntryType choose_entry(const StrategyCandidate& c, const DecisionContext& ctx);

[[nodiscard]] double round_to_tick(double price, double tick);

// Build the three geometries for a candidate. Deterministic in (candidate, ctx).
[[nodiscard]] std::vector<TradePlanVariant> generate_plans(const StrategyCandidate& c,
                                                           const DecisionContext& ctx, int64_t now_ms,
                                                           const PlanConfig& cfg = {});

} // namespace altstrategy::engine
