// MEAN_REVERSION-family (§12.9 range_extreme_mean_reversion) and CROWDING-family
// (§12.10 funding_oi_crowding_unwind). Both new, seeded RESEARCH.

#include "altstrategy/engine/strat_util.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {
namespace {

StrategyParam P(const std::string& n, double lo, double hi, double step, double def) {
    return StrategyParam{n, "double", lo, hi, step, def};
}

// -------------------------------------------------------------------------
// 12.9 range_extreme_mean_reversion  (RESEARCH)
// -------------------------------------------------------------------------
class RangeExtremeMeanReversion final : public IStrategy {
public:
    RangeExtremeMeanReversion() {
        m_.strategy_id = "range_extreme_mean_reversion";
        m_.family = Family::mean_reversion;
        m_.holding_classes = {HoldingClass::scalp, HoldingClass::intraday};
        m_.required_features = {"donchian_high_20", "donchian_low_20", "atr_14", "adx_14"};
        m_.default_lifecycle = Lifecycle::research;
        m_.explanation_template = "15m range: price at the boundary with order flow against the move";
        // `band_edge` is the fraction of the 20-bar range a price must be inside
        // to count as AT the boundary: 0.2 means the top or bottom fifth. It is
        // This parameter defines the fraction of the range considered to be
        // at its boundary.
        m_.parameter_schema = {P("band_edge", 0.05, 0.40, 0.01, 0.20), P("stop_atr", 0.8, 3.0, 0.1, 1.2),
                               P("target_atr", 0.8, 4.0, 0.1, 1.6)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& f = ctx.bitget_features;
        const double px = su::price(ctx), a = su::atr(ctx);
        if (px <= 0 || a <= 0) return std::nullopt;
        // The RANGE is a statement about the grid the channel is measured on:
        // Bitget's own 15m regime. It read the 1m venue regime AND the 1m
        // global regime -- statements about the last minutes -- for a 20-bar
        // 15m range, and added an ADX ceiling (18) and a no-expansion bound
        // (1.15) on top of a classification that already encodes both. The
        // ADX and the ATR ratio are recorded.
        const auto r15 = ctx.bitget_htf_regime.find(15);
        if (r15 == ctx.bitget_htf_regime.end() || !su::regime_is_range(r15->second.regime)) return std::nullopt;

        // WHERE price sits in the range, not how fast it got there.
        //
        // The hypothesis is a rejection AT the boundary of a range. It was
        // operationalised as RSI(14) > 72 / < 28, which measures 14 bars of
        // momentum, not position -- and momentum of that size is precisely what
        // takes a market OUT of RANGE. AltTech's classifier does not read RSI at
        // all, but it does read ADX and Bollinger width, and a run long enough to
        // push RSI past 72 pushes ADX past 25 (=> TREND) or the band percentile
        // past 0.90 (=> BREAKOUT). Measured over seven days on VPS1: RSI reached
        // an extreme in 427 contexts, none of them RANGE (0 of 1124); the five
        // that also had ADX < 18 were classified BREAKOUT (3) and TREND_UP (2).
        //
        // And when price IS at the edge of the 20-bar range inside RANGE, RSI
        // sits near the middle: mean 54.3 at the top fifth, 47.0 at the bottom.
        // So the old predicate did not merely fire rarely -- it asked for a
        // reading the market does not produce where the hypothesis applies.
        //
        // Position in the Donchian channel is the same statement the hypothesis
        // makes, in the terms the hypothesis makes it.
        const double hi = f.get("donchian_high_20", 0.0), lo = f.get("donchian_low_20", 0.0);
        if (!(hi > lo) || !std::isfinite(hi) || !std::isfinite(lo)) return std::nullopt;
        const double pos = (px - lo) / (hi - lo);          // 0 at the low, 1 at the high
        const double edge = p.get("band_edge", 0.20);

        // The exhaustion reading is a hard condition below; absent, its 0
        // default satisfied both `<= 0` and `>= 0`. Missing is unknown.
        if (!ctx.microstructure.has("trade_imbalance")) return std::nullopt;
        const double exhaustion = ctx.microstructure.get("trade_imbalance");
        Side side = Side::unspecified;
        // Fade the boundary toward the range mid: at resistance → short. The
        // boundary is the channel's edge band; a projected zone within 0.7 ATR
        // was a second, hand-set definition of the same boundary and is now
        // recorded (room_to_zone_atr is the distance to the zone BEHIND the
        // fade's target; zone_at_boundary_atr the one price is rejecting).
        if (pos >= 1.0 - edge && exhaustion <= 0) side = Side::short_;
        else if (pos <= edge && exhaustion >= 0) side = Side::long_;
        if (side == Side::unspecified) return std::nullopt;
        const double adx = f.get("adx_14", NAN);

        // No model gate here. Whether this setup pays is the economic
        // policy's question (economic_policy.hpp), asked once, of a calibrated
        // estimate for the setup's horizon. A per-strategy veto on a raw
        // probability (the 0.55 / 0.6 cuts that stood here) was a hand-set
        // boundary on a number that meant something else.

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 1.2), p.get("target_atr", 1.6));
        s.holding_class = HoldingClass::scalp;
        s.expected_horizon_ms = 60LL * 60 * 1000;
        s.cross_venue_confirmation = 0.0;
        // Conviction from how far into the edge band price sits, and from how
        // little trend there is -- the two things the hypothesis rests on.
        const double edge_depth = side == Side::short_ ? pos - (1.0 - edge) : edge - pos;
        s.raw_setup_score = std::clamp(0.4 + su::conviction(edge_depth, edge) * 0.6, 0.0, 1.0);
        s.reason_codes = {"range_boundary_rejection", "order_flow_exhaustion"};
        su::put_context(s, ctx);
        su::put(s, "range_position", pos);
        su::put(s, "adx_14_15m", adx);
        if (const auto* z = ctx.nearest_zone(side == Side::short_ ? "resistance" : "support", px))
            su::put(s, "zone_at_boundary_atr", std::abs(z->center - px) / a);
        return s;
    }

private:
    StrategyMeta m_;
};

// -------------------------------------------------------------------------
// 12.10 funding_oi_crowding_unwind  (RESEARCH). Horizon up to 48h.
// -------------------------------------------------------------------------
class FundingOiCrowdingUnwind final : public IStrategy {
public:
    FundingOiCrowdingUnwind() {
        m_.strategy_id = "funding_oi_crowding_unwind";
        m_.family = Family::crowding;
        m_.holding_classes = {HoldingClass::intraday, HoldingClass::extended_intraday};
        m_.required_features = {"funding_divergence", "oi_growth_1h", "atr_14"};
        m_.default_lifecycle = Lifecycle::research;
        m_.max_candidate_freq_ms = 300'000;
        m_.explanation_template = "Crowded funding/OI extreme unwind";
        // funding_extreme and oi_growth_min define the crowded-positioning
        // event.
        m_.parameter_schema = {P("funding_extreme", 0.0003, 0.005, 0.0001, 0.0008),
                               P("oi_growth_min", 0.02, 0.5, 0.01, 0.08),
                               P("stop_atr", 1.5, 5.0, 0.1, 2.5), P("target_atr", 2.0, 10.0, 0.1, 4.0)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& cv = ctx.cross_venue_features;
        const double px = su::price(ctx), a = su::atr(ctx);
        const double funding = ctx.funding_rate;
        const double funding_div = cv.get("funding_divergence");   // Bitget funding vs consensus
        // ACCUMULATED growth over the crowding window, not the one-minute delta
        // AltTech publishes. `oi_growth` is (OI[t] - OI[t-1min]) / OI[t-1min],
        // whose 7-day maximum is 0.0616 -- the 0.08 gate below could not open
        // once. Crowding is what those deltas compound to, which crosses 0.08
        // about four times in three days: rare, as an extreme should be.
        const double oi_growth = cv.get("oi_growth_1h");
        if (!cv.has("oi_growth_1h")) return std::nullopt;   // absent, never assumed
        // The same for every other hard condition. `ctx.funding_rate` is the
        // consensus funding with a 0 default, and a CVD of 0 passed both
        // `<= 0` and `>= 0`: a missing confirmation confirmed either side.
        if (!cv.has("funding_rate_8h") || !ctx.microstructure.has("cvd_delta") ||
            !ctx.bitget_features.has("price_stall"))
            return std::nullopt;
        const double cvd = ctx.microstructure.get("cvd_delta");
        const double price_stall = ctx.bitget_features.get("price_stall", 0); // 1 when momentum flat despite OI up
        if (px <= 0 || a <= 0) return std::nullopt;
        if (std::abs(funding) < p.get("funding_extreme", 0.0008)) return std::nullopt;
        if (oi_growth < p.get("oi_growth_min", 0.08)) return std::nullopt;

        Side side = Side::unspecified;
        // Crowded longs (funding very positive) + OI up + stalling price + weakening CVD → short unwind.
        if (funding > 0 && price_stall > 0 && cvd <= 0 && funding_div >= 0) side = Side::short_;
        else if (funding < 0 && price_stall > 0 && cvd >= 0 && funding_div <= 0) side = Side::long_;
        if (side == Side::unspecified) return std::nullopt;

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 2.5), p.get("target_atr", 4.0));
        s.holding_class = HoldingClass::extended_intraday;
        s.expected_horizon_ms = 24LL * 3600 * 1000;  // up to 48h, capped by plan (§25)
        s.cross_venue_confirmation = std::clamp(std::abs(funding_div) * 100.0, 0.0, 1.0);
        s.raw_setup_score = std::clamp(0.4 + su::conviction(std::abs(funding), 0.002) * 0.3 +
                                       su::conviction(oi_growth, 0.3) * 0.3, 0.0, 1.0);
        s.reason_codes = {"funding_extreme", "oi_growth", "price_stall", "cvd_weakening"};
        su::put_context(s, ctx);
        su::put(s, "funding_rate_8h", funding);
        su::put(s, "funding_divergence", funding_div);
        su::put(s, "oi_growth_1h", oi_growth);
        return s;
    }

private:
    StrategyMeta m_;
};

} // namespace

ALTSTRATEGY_V2_REGISTER_STRATEGY(RangeExtremeMeanReversion)
ALTSTRATEGY_V2_REGISTER_STRATEGY(FundingOiCrowdingUnwind)

} // namespace altstrategy::engine
