// Book- and flow-based hypotheses. These read the resting book and the executed
// tape rather than the candle history, so they are mechanically independent of
// the trend/breakout half of the catalogue: a strategy here can be right on a
// bar where every indicator-derived strategy is wrong, which is the entire
// point of measuring family independence in the ensemble.
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
// book_imbalance_pressure  (RESEARCH)
//
// Hypothesis: over a horizon of minutes, price drifts toward the heavier side
// of the resting book. The measurable is the depth imbalance at the top of the
// book, corroborated by the microprice sitting off the mid in the same
// direction. Cont/Kukanov/Stoikov (2014) report a near-linear relation between
// order-flow imbalance and short-horizon returns; this is the standing-depth
// version of that statement.
//
// Deliberately NOT a momentum strategy: it takes no view on the candle history
// and can point against the prevailing trend.
// -------------------------------------------------------------------------
class BookImbalancePressure final : public IStrategy {
public:
    BookImbalancePressure() {
        m_.strategy_id = "book_imbalance_pressure";
        m_.family = Family::microstructure;
        m_.holding_classes = {HoldingClass::scalp};
        m_.required_features = {"book_imbalance", "book_microprice_dev_bps", "atr_14"};
        m_.required_quality = ContextQuality::degraded_3_of_4;
        m_.default_lifecycle = Lifecycle::research;
        m_.max_candidate_freq_ms = 60'000;
        m_.explanation_template = "Resting-book depth imbalance with the microprice off the mid the same way";
        // imbalance_min is DEFINITIONAL: (bid - ask) / (bid + ask) >= 1/3 is
        // exactly "one side holds at least twice the other's depth". The
        // legacy 0.25 (5:3) had no stated origin and, without the quote-size
        // filter beside it, held in ~20 % of contexts -- a state, not an event.
        m_.parameter_schema = {P("imbalance_min", 0.10, 0.80, 0.01, 1.0 / 3.0),
                               P("stop_atr", 0.4, 2.0, 0.1, 0.8),
                               P("target_atr", 0.4, 3.0, 0.1, 1.0)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& mi = ctx.microstructure;
        const double px = su::price(ctx), a = su::atr(ctx);
        if (px <= 0 || a <= 0) return std::nullopt;
        // Both inputs must be MEASURED. A book-pressure trade taken on the
        // conservative stand-in spread would be a trade on an assumption.
        if (!ctx.spread_observed || !ctx.depth_observed) return std::nullopt;
        if (!mi.has("book_imbalance") || !mi.has("book_microprice_dev_bps")) return std::nullopt;
        // A 6 bps spread cap stood here: whether a wide book is worth trading
        // is an execution judgement (VPS2's). The spread is recorded.

        const double imb = mi.get("book_imbalance");        // (bid-ask)/(bid+ask), -1..+1
        // In BASIS POINTS: the producer publishes a ratio, and the threshold
        // below is written in bps. Reading the ratio here compared two
        // quantities 10 000 apart and the gate could never open.
        const double micro_dev = mi.get("book_microprice_dev_bps");  // bps, signed off the mid
        const double imb_min = p.get("imbalance_min", 1.0 / 3.0);
        // The microprice corroborates by its SIGN: it lies inside the spread by
        // construction, so a hand-set minimum distance (0.5 bps) was a bound on
        // quote size, not on the imbalance.
        Side side = Side::unspecified;
        if (imb >= imb_min && micro_dev > 0) side = Side::long_;
        else if (imb <= -imb_min && micro_dev < 0) side = Side::short_;
        if (side == Side::unspecified) return std::nullopt;

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 0.8), p.get("target_atr", 1.0));
        s.holding_class = HoldingClass::scalp;
        s.expected_horizon_ms = 20LL * 60 * 1000;
        s.cross_venue_confirmation =
            std::clamp(std::abs(ctx.cross_venue_features.get("breakout_agreement", 0.0)), 0.0, 1.0);
        s.raw_setup_score = std::clamp(0.35 + su::conviction(imb, 0.5) * 0.4 +
                                       su::conviction(micro_dev, 3.0) * 0.25, 0.0, 1.0);
        s.reason_codes = {"book_depth_imbalance", "microprice_confirms"};
        su::put_context(s, ctx);
        su::put(s, "book_imbalance", imb);
        su::put(s, "book_microprice_dev_bps", micro_dev);
        return s;
    }

private:
    StrategyMeta m_;
};

// -------------------------------------------------------------------------
// cvd_absorption_reversal  (RESEARCH)
//
// Hypothesis: a new price extreme that the executed tape does NOT confirm is
// being absorbed by a passive participant, and reverts. The measurable is a
// Donchian extreme reached while cumulative volume delta moves the other way.
//
// This is the opposite conclusion to volume_weighted_momentum_burst on a
// related input, and that is deliberate: the two are separated by whether flow
// CONFIRMS the move. Burst requires cvd with the move; absorption requires cvd
// against it. They cannot both fire on the same bar.
// -------------------------------------------------------------------------
class CvdAbsorptionReversal final : public IStrategy {
public:
    CvdAbsorptionReversal() {
        m_.strategy_id = "cvd_absorption_reversal";
        m_.family = Family::order_flow;
        m_.holding_classes = {HoldingClass::scalp, HoldingClass::intraday};
        m_.required_features = {"donchian_high_20", "donchian_low_20", "cvd_delta", "atr_14"};
        m_.default_lifecycle = Lifecycle::research;
        m_.explanation_template = "Price at the 20-bar extreme while executed flow runs the other way";
        m_.parameter_schema = {P("stop_atr", 0.6, 3.0, 0.1, 1.1),
                               P("target_atr", 0.6, 4.0, 0.1, 1.5)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& f = ctx.bitget_features;
        const auto& mi = ctx.microstructure;
        const double px = su::price(ctx), a = su::atr(ctx);
        const double hi = f.get("donchian_high_20", 0), lo = f.get("donchian_low_20", 0);
        if (px <= 0 || a <= 0 || hi <= 0 || lo <= 0) return std::nullopt;
        if (!mi.has("cvd_delta")) return std::nullopt;

        const double cvd = mi.get("cvd_delta");
        // trade_imbalance is a hard condition below; its 0 default satisfied
        // both `<= 0` and `>= 0`, so an absent reading confirmed either side.
        if (!mi.has("trade_imbalance")) return std::nullopt;
        const double imb = mi.get("trade_imbalance");
        // "At the extreme" within quote noise (half the observed spread; it was
        // a hand-set 1 bps), and flow AGAINST the move by sign -- the 0.20 CVD
        // floor was a hand-set strength bound; the CVD is recorded.
        const double buf = px * su::noise_bound_bps(ctx) / 10'000.0;

        Side side = Side::unspecified;
        // New high, but the tape is net selling into it → the high is offered.
        if (px >= hi - buf && cvd < 0 && imb <= 0) side = Side::short_;
        // New low, but the tape is net buying into it → the low is bid.
        else if (px <= lo + buf && cvd > 0 && imb >= 0) side = Side::long_;
        if (side == Side::unspecified) return std::nullopt;

        // No model gate here. Whether this setup pays is the economic
        // policy's question (economic_policy.hpp), asked once, of a calibrated
        // estimate for the setup's horizon. A per-strategy veto on a raw
        // probability (the 0.55 / 0.6 cuts that stood here) was a hand-set
        // boundary on a number that meant something else.

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 1.1), p.get("target_atr", 1.5));
        s.holding_class = HoldingClass::scalp;
        s.expected_horizon_ms = 45LL * 60 * 1000;
        s.cross_venue_confirmation = 0.0;   // an absorption is venue-local by construction
        s.raw_setup_score = std::clamp(0.35 + su::conviction(cvd, 1.0) * 0.45, 0.0, 1.0);
        s.reason_codes = {"price_extreme", "flow_does_not_confirm", "absorption"};
        su::put_context(s, ctx);
        su::put(s, "extreme_distance_atr", side == Side::short_ ? (hi - px) / a : (px - lo) / a);
        return s;
    }

private:
    StrategyMeta m_;
};

// -------------------------------------------------------------------------
// spread_dislocation_fade  (RESEARCH)
//
// Hypothesis: a spread that widens sharply while depth stays intact is a
// transient withdrawal of quotes, not a change in fair value, and the price
// print made during it reverts. The measurable is spread_bps against the venue's
// liquidity rank with depth still present.
//
// The guard that makes this honest: it refuses to act unless BOTH spread and
// depth are measured, because the fallback spread is by construction wide, and
// a strategy that fires on the fallback would fire precisely when it is blind.
// -------------------------------------------------------------------------
class SpreadDislocationFade final : public IStrategy {
public:
    SpreadDislocationFade() {
        m_.strategy_id = "spread_dislocation_fade";
        m_.family = Family::liquidity;
        m_.holding_classes = {HoldingClass::scalp};
        m_.required_features = {"spread_bps", "book_bid_depth", "book_ask_depth", "atr_14"};
        m_.required_quality = ContextQuality::degraded_3_of_4;
        m_.default_lifecycle = Lifecycle::research;
        m_.explanation_template = "Transient quote withdrawal with depth intact";
        // spread_spike_bps and min_depth_usdt are DEFINITIONAL (what a spike
        // and "depth intact" mean): absolute thresholds on the instrument's
        // book values.
        m_.parameter_schema = {P("spread_spike_bps", 3.0, 40.0, 0.5, 9.0),
                               P("min_depth_usdt", 1000.0, 500000.0, 1000.0, 25000.0),
                               P("stop_atr", 0.4, 2.0, 0.1, 0.9),
                               P("target_atr", 0.4, 2.5, 0.1, 1.0)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const double px = su::price(ctx), a = su::atr(ctx);
        if (px <= 0 || a <= 0) return std::nullopt;
        if (!ctx.spread_observed || !ctx.depth_observed) return std::nullopt;
        if (ctx.spread_bps < p.get("spread_spike_bps", 9.0)) return std::nullopt;
        if (ctx.top_depth_usdt < p.get("min_depth_usdt", 25000.0)) return std::nullopt;
        // The liquidity-rank floor that stood here ranked Bitget's book against
        // the other venues' -- whether the book is good enough to trade is an
        // execution judgement. The score is recorded (put_context).

        // Fade the direction the last print pushed, read from the microprice:
        // a microprice above the mid means the print was lifted, so fade short.
        const double micro_dev = ctx.microstructure.get("book_microprice_dev_bps", 0);
        if (std::abs(micro_dev) < 1e-9) return std::nullopt;
        const Side side = micro_dev > 0 ? Side::short_ : Side::long_;

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 0.9), p.get("target_atr", 1.0));
        s.holding_class = HoldingClass::scalp;
        s.expected_horizon_ms = 15LL * 60 * 1000;
        s.cross_venue_confirmation = 0.0;
        s.raw_setup_score = std::clamp(0.30 + su::conviction(ctx.spread_bps, 20.0) * 0.35 +
                                       su::conviction(micro_dev, 5.0) * 0.25, 0.0, 1.0);
        s.reason_codes = {"spread_spike", "depth_intact", "measured_book"};
        su::put_context(s, ctx);
        su::put(s, "book_microprice_dev_bps", micro_dev);
        return s;
    }

private:
    StrategyMeta m_;
};

} // namespace

ALTSTRATEGY_V2_REGISTER_STRATEGY(BookImbalancePressure)
ALTSTRATEGY_V2_REGISTER_STRATEGY(CvdAbsorptionReversal)
ALTSTRATEGY_V2_REGISTER_STRATEGY(SpreadDislocationFade)

} // namespace altstrategy::engine
