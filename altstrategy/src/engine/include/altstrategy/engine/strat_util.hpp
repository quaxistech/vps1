#pragma once

// Shared authoring helpers for the strategy catalogue. Keeps the ten strategy
// bodies focused on their edge rather than on geometry bookkeeping, and gives a
// single canonical feature vocabulary (documented in docs/DECISION_CONTEXT.md)
// that both the context_assembler and the strategies agree on.

#include "altstrategy/engine/decision_types.hpp"
#include "altstrategy/engine/strategy.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine::su {

// ATR-anchored geometry. LONG: stop below, target above; SHORT mirrored.
inline void geo_from_atr(RawSignal& sig, Side side, double price, double atr,
                         double stop_mult, double target_mult) {
    if (!std::isfinite(price) || price <= 0 || !std::isfinite(atr) || atr <= 0 ||
        !std::isfinite(stop_mult) || stop_mult <= 0 || !std::isfinite(target_mult) || target_mult <= 0)
        return; // no geometry rather than an invented0.4% volatility
    const double buffer = atr;
    sig.entry_hint = price;
    if (side == Side::long_) {
        sig.invalidation_hint = price - stop_mult * buffer;
        sig.target_hint = price + target_mult * buffer;
    } else {
        sig.invalidation_hint = price + stop_mult * buffer;
        sig.target_hint = price - target_mult * buffer;
    }
}

// Raw reward/risk of a RawSignal's hints (0 if degenerate/inverted).
inline double raw_rr(const RawSignal& s) {
    if (s.entry_hint <= 0 || s.invalidation_hint <= 0 || s.target_hint <= 0) return 0;
    const bool is_long = s.side == Side::long_;
    if (is_long && !(s.invalidation_hint < s.entry_hint && s.entry_hint < s.target_hint)) return 0;
    if (!is_long && !(s.invalidation_hint > s.entry_hint && s.entry_hint > s.target_hint)) return 0;
    const double risk = std::abs(s.entry_hint - s.invalidation_hint);
    return risk > 0 ? std::abs(s.target_hint - s.entry_hint) / risk : 0;
}

inline bool regime_is_trend_up(const std::string& r) { return r == "TREND_UP"; }
inline bool regime_is_trend_down(const std::string& r) { return r == "TREND_DOWN"; }
inline bool regime_is_range(const std::string& r) { return r == "RANGE"; }

// Normalize an unbounded z-like value to a 0..1 conviction via a soft cap.
inline double conviction(double x, double scale) {
    if (scale <= 0) return 0;
    return std::clamp(std::tanh(std::abs(x) / scale), 0.0, 1.0);
}

// Bitget close feature or mark fallback.
inline double price(const DecisionContext& ctx) { return ctx.last_price(); }
inline double atr(const DecisionContext& ctx) { return ctx.atr(); }

// The smallest price difference that is a MOVE rather than quote noise, in
// bps: half the observed bid/ask spread. A reference price closer than that to
// a level has not crossed it in any sense a trade could, since the quote
// itself straddles the level. It replaces the hand-set 1-4 bps "buffers" and
// "minimum dislocations" the detectors carried (legacy numbers with no stated
// origin). Unobserved spread: no bound can be derived, so 0 -- the setup's own
// definition, a strict crossing -- and `spread_observed` says so on the
// candidate.
inline double noise_bound_bps(const DecisionContext& ctx) {
    return ctx.spread_observed && std::isfinite(ctx.spread_bps) && ctx.spread_bps > 0
               ? 0.5 * ctx.spread_bps
               : 0.0;
}

// Bitget's deviation from the cross-venue consensus is a DISLOCATION when it
// is a statistical outlier against the venues' own disagreement: more than two
// dispersions away (the conventional two-sigma bound), and more than quote
// noise. Measured on VPS1 (24 h, 2 573 contexts, 2026-09-26): |dev| exceeded
// half the spread in 69 % of contexts and the dispersion in 53 % -- states,
// not events; two dispersions in 5.7 %.
inline bool dislocated(const DecisionContext& ctx, double dev_bps) {
    const auto& xv = ctx.cross_venue_features;
    if (!std::isfinite(dev_bps) || !xv.has("venue_price_dispersion_bps")) return false;
    const double disp = xv.get("venue_price_dispersion_bps");
    if (!std::isfinite(disp) || disp < 0) return false;
    const double a = std::abs(dev_bps);
    return a > 2.0 * disp && a > noise_bound_bps(ctx);
}

// Record a descriptor only when it is a measurement.
inline void put(RawSignal& s, const std::string& name, double v) {
    if (std::isfinite(v)) s.descriptors[name] = v;
}
inline void put_if(RawSignal& s, const FeatureSummary& f, const std::string& name,
                   const std::string& as = {}) {
    if (f.has(name)) put(s, as.empty() ? name : as, f.get(name));
}

// Distance to the nearest projected zone of a kind, in ATR units; recorded as
// context. It used to be a veto ("needs 1.2 ATR of room"), a trading judgement
// about where a target could sit.
inline void put_room(RawSignal& s, const DecisionContext& ctx, Side side, double px, double a) {
    if (!(a > 0)) return;
    const std::string kind = side == Side::long_ ? "resistance" : "support";
    if (const auto* z = ctx.nearest_zone(kind, px)) put(s, "room_to_zone_atr", std::abs(z->center - px) / a);
}

// Context every setup carries: the book, the regimes on each grid, the
// venue agreement. The same names on every candidate, whichever setup fired.
inline void put_context(RawSignal& s, const DecisionContext& ctx) {
    if (ctx.spread_observed) put(s, "spread_bps", ctx.spread_bps);
    if (ctx.depth_observed) put(s, "top_depth_usdt", ctx.top_depth_usdt);
    if (const auto l = ctx.liquidity_score()) put(s, "liquidity_score", *l);
    put_if(s, ctx.bitget_features, "momentum_direction_agreement_15m");
    put_if(s, ctx.cross_venue_features, "momentum_direction_agreement");
    put_if(s, ctx.bitget_features, "atr_expansion");
    put_if(s, ctx.microstructure, "volume_surge");
    put_if(s, ctx.microstructure, "cvd_delta");
    put_if(s, ctx.microstructure, "trade_imbalance");
    const double a = atr(ctx);
    if (a > 0 && price(ctx) > 0) put(s, "atr_bps", a / price(ctx) * 10'000.0);
}

} // namespace altstrategy::engine::su
