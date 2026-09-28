// Carry, cross-venue convergence, and multi-timeframe structure. None of these
// read momentum: each states a relation that should hold between two observable
// prices (or two grids) and trades its violation.
//
// Sources are recorded in docs/STRATEGY_SOURCES.md.

#include "altstrategy/engine/strat_util.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {
namespace {

StrategyParam P(const std::string& n, double lo, double hi, double step, double def) {
    return StrategyParam{n, "double", lo, hi, step, def};
}

// -------------------------------------------------------------------------
// funding_basis_convergence  (RESEARCH)
//
// Hypothesis: a perpetual whose mark price stands far from its index while
// funding is paying the same direction is carrying a crowded position, and the
// basis converges toward the index as funding is paid. The measurable is the
// signed mark-index basis in bps together with the 8h funding rate.
//
// Distinct from funding_oi_crowding_unwind, which needs open interest to be
// GROWING and price to have stalled — an exhaustion statement about
// positioning. This one is a pricing statement about two prices, and fires on
// bars where open interest is flat.
// -------------------------------------------------------------------------
class FundingBasisConvergence final : public IStrategy {
public:
    FundingBasisConvergence() {
        m_.strategy_id = "funding_basis_convergence";
        m_.family = Family::basis;
        m_.holding_classes = {HoldingClass::intraday, HoldingClass::extended_intraday};
        m_.required_features = {"atr_14"};
        m_.default_lifecycle = Lifecycle::research;
        m_.explanation_template = "Mark-index basis with funding paying the same way";
        // basis_min_bps and funding_min_8h define the basis and funding events.
        m_.parameter_schema = {P("basis_min_bps", 2.0, 60.0, 0.5, 8.0),
                               P("funding_min_8h", 0.0001, 0.0100, 0.0001, 0.0006),
                               P("stop_atr", 1.0, 4.0, 0.1, 1.8),
                               P("target_atr", 1.0, 5.0, 0.1, 2.2)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const double a = su::atr(ctx);
        // Both legs must be real prices. index_price is 0 when AltSpotter's
        // derivatives context has not arrived, and a basis measured against
        // zero is not a small basis -- it is no measurement at all.
        // The basis is (mark - index), and both legs must be the real thing.
        // `ctx.mark_price` is the consensus MID -- that is what last_price()
        // needs and what the field has always held -- so the mark is read from
        // the vector member that actually is a mark. Reading the mid here made
        // the basis a mid-to-index spread, which is a different quantity that
        // happens to look plausible.
        const double mark = ctx.cross_venue_features.get("consensus_mark_price", 0.0);
        if (mark <= 0 || ctx.index_price <= 0 || a <= 0) return std::nullopt;

        const double basis_bps = (mark - ctx.index_price) / ctx.index_price * 10'000.0;
        // The consensus 8h funding across venues, falling back to the venue's
        // own rate only when the cross-venue member is absent.
        const double funding = ctx.cross_venue_features.has("funding_rate_8h")
                                   ? ctx.cross_venue_features.get("funding_rate_8h")
                                   : ctx.funding_rate;
        if (std::abs(funding) < p.get("funding_min_8h", 0.0006)) return std::nullopt;
        const double min_basis = p.get("basis_min_bps", 8.0);

        Side side = Side::unspecified;
        // Mark above index AND longs paying: the premium is crowded → short.
        if (basis_bps >= min_basis && funding > 0) side = Side::short_;
        else if (basis_bps <= -min_basis && funding < 0) side = Side::long_;
        if (side == Side::unspecified) return std::nullopt;

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, ctx.mark_price, a, p.get("stop_atr", 1.8), p.get("target_atr", 2.2));
        s.holding_class = HoldingClass::extended_intraday;
        // One funding interval. Convergence is paid by funding, so the horizon
        // is the settlement clock, not a price target's arrival time.
        s.expected_horizon_ms = 8LL * 3600 * 1000;
        s.cross_venue_confirmation =
            std::clamp(std::abs(ctx.cross_venue_features.get("cross_venue_confidence", 0.0)), 0.0, 1.0);
        s.raw_setup_score = std::clamp(0.35 + su::conviction(basis_bps, 20.0) * 0.35 +
                                       su::conviction(funding, 0.002) * 0.30, 0.0, 1.0);
        s.reason_codes = {"mark_index_basis", "funding_same_direction", "carry_convergence"};
        su::put_context(s, ctx);
        su::put(s, "basis_bps", basis_bps);
        su::put(s, "funding_rate_8h", funding);
        return s;
    }

private:
    StrategyMeta m_;
};

// -------------------------------------------------------------------------
// venue_dislocation_convergence  (RESEARCH)
//
// Hypothesis: when Bitget's price stands away from the cross-venue consensus
// and Bitget is NOT the venue leading the move, the gap closes toward consensus.
//
// The distinction from cross_venue_lead_lag_follow is the whole hypothesis:
// that strategy follows the leader's move, this one fades the follower's
// overshoot, and the lead score decides which of the two applies. They are
// mutually exclusive on any given bar by construction.
// -------------------------------------------------------------------------
class VenueDislocationConvergence final : public IStrategy {
public:
    VenueDislocationConvergence() {
        m_.strategy_id = "venue_dislocation_convergence";
        m_.family = Family::cross_venue;
        m_.holding_classes = {HoldingClass::scalp, HoldingClass::intraday};
        m_.required_features = {"atr_14"};
        // Convergence to a consensus is meaningless if the consensus is one
        // venue: require the reference set to be genuinely multi-venue (a data
        // quality requirement, not a trading filter).
        m_.required_quality = ContextQuality::degraded_3_of_4;
        m_.default_lifecycle = Lifecycle::research;
        m_.explanation_template = "Bitget away from consensus while not leading";
        m_.parameter_schema = {P("stop_atr", 0.5, 2.5, 0.1, 1.0),
                               P("target_atr", 0.5, 3.0, 0.1, 1.2)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& xv = ctx.cross_venue_features;
        const double px = su::price(ctx), a = su::atr(ctx);
        if (px <= 0 || a <= 0) return std::nullopt;
        // Bitget's OWN deviation from the consensus, as the producer measures it.
        //
        // This computed `(px - consensus) / consensus` where `px` is
        // ctx.last_price() -- which returns mark_price, which the assembler
        // fills from consensus_mid_price. The gap was therefore the consensus
        // compared with itself: identically zero on every context ever built, so
        // the `>= 4 bps` gate could not open and the strategy produced 0
        // candidates for its entire deployed life. AltTech publishes the real
        // quantity as `bitget_mid_deviation_bps`, signed, and it crosses 4 bps in
        // 22% of vectors.
        if (!xv.has("bitget_mid_deviation_bps") || !xv.has("bitget_lead_score")) {
            return std::nullopt;
        }

        // Bitget leading its own dislocation means the other venues have not
        // repriced YET -- that is the lead-lag setup, not this one. "Not
        // leading" is a sign (the 0.40 allowance on a -5..+5 score was
        // hand-set), and the gap must exceed quote noise: half the observed
        // spread, where a hand-set 4 bps stood.
        const double lead = xv.get("bitget_lead_score");
        if (lead > 0) return std::nullopt;

        const double gap_bps = xv.get("bitget_mid_deviation_bps");
        if (!std::isfinite(gap_bps)) return std::nullopt;
        // Bitget must be an OUTLIER against the consensus venues' own
        // disagreement: beyond two dispersions and beyond quote noise
        // (strat_util.hpp::dislocated). "Beyond one dispersion" held in 53 % of
        // contexts -- a state, not a dislocation. Unmeasured dispersion is no
        // dislocation: "consensus" is then not known to mean anything.
        if (!su::dislocated(ctx, gap_bps)) return std::nullopt;
        const double dispersion = xv.get("venue_price_dispersion_bps");

        const Side side = gap_bps > 0 ? Side::short_ : Side::long_;

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 1.0), p.get("target_atr", 1.2));
        s.holding_class = HoldingClass::scalp;
        s.expected_horizon_ms = 30LL * 60 * 1000;
        s.cross_venue_confirmation = std::clamp(xv.get("cross_venue_confidence", 0.0), 0.0, 1.0);
        s.raw_setup_score = std::clamp(0.35 + su::conviction(gap_bps, 12.0) * 0.45, 0.0, 1.0);
        s.reason_codes = {"bitget_off_consensus", "bitget_not_leading", "convergence"};
        su::put_context(s, ctx);
        su::put(s, "bitget_mid_deviation_bps", gap_bps);
        su::put(s, "bitget_lead_score", lead);
        su::put(s, "venue_price_dispersion_bps", dispersion);
        return s;
    }

private:
    StrategyMeta m_;
};

// -------------------------------------------------------------------------
// htf_range_false_breakout_fade  (RESEARCH)
//
// Hypothesis: an execution-grid breakout that occurs while the HIGHER grid is
// still ranging is a liquidity sweep, not a trend start, and reverts into the
// higher grid's range.
//
// This is the catalogue's only strategy whose premise lives on a different
// timeframe from its trigger, so it is the one that genuinely exercises the
// multi-timeframe context. It refuses when the higher grid is absent rather
// than substituting the execution grid: a 4h range and a 15m range are
// different claims, and reading one for the other is how a fade gets taken
// against a real trend.
// -------------------------------------------------------------------------
class HtfRangeFalseBreakoutFade final : public IStrategy {
public:
    HtfRangeFalseBreakoutFade() {
        m_.strategy_id = "htf_range_false_breakout_fade";
        m_.family = Family::mean_reversion;
        m_.holding_classes = {HoldingClass::intraday};
        m_.required_features = {"donchian_high_20_prev", "donchian_low_20_prev", "atr_14"};
        m_.default_lifecycle = Lifecycle::research;
        m_.explanation_template = "15m break while the 4h grid still ranges";
        // htf_adx_max = 20 is Wilder's conventional ADX boundary for a ranging
        // market, rather than a fitted value.
        m_.parameter_schema = {P("context_minutes", 60, 1440, 1, 240),
                               P("htf_adx_max", 8, 30, 1, 20),
                               P("stop_atr", 0.8, 3.0, 0.1, 1.3),
                               P("target_atr", 0.8, 4.0, 0.1, 1.8)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& f = ctx.bitget_features;
        const double px = su::price(ctx), a = su::atr(ctx);
        // A break of the execution grid's range is a break of the range the
        // PREVIOUS bars made -- the channel including the bar just closed
        // already contains the break itself (see donchian_breakout_continuation).
        const double hi = f.get("donchian_high_20_prev", 0), lo = f.get("donchian_low_20_prev", 0);
        if (px <= 0 || a <= 0 || hi <= 0 || lo <= 0) return std::nullopt;

        const auto grid = static_cast<int32_t>(p.get("context_minutes", 240));
        const auto it = ctx.context_features.find(grid);
        if (it == ctx.context_features.end() || !it->second.valid) return std::nullopt;
        const FeatureSummary& htf = it->second;
        if (!htf.has("adx_14") || !htf.has("donchian_high_20") || !htf.has("donchian_low_20"))
            return std::nullopt;

        // The higher grid must actually be ranging.
        if (htf.get("adx_14") > p.get("htf_adx_max", 20)) return std::nullopt;
        const double htf_hi = htf.get("donchian_high_20"), htf_lo = htf.get("donchian_low_20");
        if (htf_hi <= 0 || htf_lo <= htf_hi * 0.0) return std::nullopt;

        // Beyond the channel by more than quote noise (half the observed
        // spread; a hand-set 2 bps stood here).
        const double buf = px * su::noise_bound_bps(ctx) / 10'000.0;
        Side side = Side::unspecified;
        // Broke the execution-grid high, but is still inside the higher range.
        if (px > hi + buf && px < htf_hi) side = Side::short_;
        else if (px < lo - buf && px > htf_lo) side = Side::long_;
        if (side == Side::unspecified) return std::nullopt;

        // Target the higher grid's mid, but never further than the ATR-based
        // geometry would allow: the range mid can be hours away.
        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 1.3), p.get("target_atr", 1.8));
        //
        // The mid is a target only when it lies on the TRADE's side of price --
        // below a short's entry, above a long's -- and then the nearer of the two
        // wins. The comparisons used to be inverted: a mid in the trade's
        // direction was never applied, and a mid on the wrong side became the
        // target, putting a short's target above its entry.
        const double htf_mid = 0.5 * (htf_hi + htf_lo);
        if (side == Side::short_ && htf_mid < px) s.target_hint = std::max(s.target_hint, htf_mid);
        if (side == Side::long_ && htf_mid > px) s.target_hint = std::min(s.target_hint, htf_mid);
        s.holding_class = HoldingClass::intraday;
        s.expected_horizon_ms = 4LL * 3600 * 1000;
        s.cross_venue_confirmation = 0.0;
        s.raw_setup_score = std::clamp(0.35 + su::conviction(htf.get("adx_14") > 0
                                                                 ? (p.get("htf_adx_max", 20) - htf.get("adx_14"))
                                                                 : 0.0,
                                                             10.0) * 0.3,
                                       0.0, 1.0);
        s.reason_codes = {"ltf_break", "htf_still_ranging", "sweep_fade"};
        su::put_context(s, ctx);
        su::put(s, "htf_adx_14", htf.get("adx_14"));
        su::put(s, "break_distance_atr", side == Side::short_ ? (px - hi) / a : (lo - px) / a);
        if (htf_hi > htf_lo) su::put(s, "htf_range_position", (px - htf_lo) / (htf_hi - htf_lo));
        return s;
    }

private:
    StrategyMeta m_;
};

} // namespace

ALTSTRATEGY_V2_REGISTER_STRATEGY(FundingBasisConvergence)
ALTSTRATEGY_V2_REGISTER_STRATEGY(VenueDislocationConvergence)
ALTSTRATEGY_V2_REGISTER_STRATEGY(HtfRangeFalseBreakoutFade)

} // namespace altstrategy::engine
