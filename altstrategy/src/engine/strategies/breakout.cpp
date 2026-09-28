// BREAKOUT-family setups (§12.5 donchian_breakout_continuation,
// §12.6 ema_compression_expansion): event detectors. The event definition
// produces a candidate; context and named descriptors accompany it downstream.

#include "altstrategy/engine/strat_util.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {
namespace {

StrategyParam P(const std::string& n, double lo, double hi, double step, double def) {
    return StrategyParam{n, "double", lo, hi, step, def};
}

// -------------------------------------------------------------------------
// 12.5 donchian_breakout_continuation  (SHADOW)
// -------------------------------------------------------------------------
class DonchianBreakoutContinuation final : public IStrategy {
public:
    DonchianBreakoutContinuation() {
        m_.strategy_id = "donchian_breakout_continuation";
        m_.family = Family::breakout;
        m_.holding_classes = {HoldingClass::intraday};
        m_.required_features = {"donchian_high_20_prev", "donchian_low_20_prev", "atr_14"};
        m_.default_lifecycle = Lifecycle::shadow;
        m_.explanation_template = "15m close beyond the prior 20-bar Donchian channel";
        m_.parameter_schema = {P("stop_atr", 1.0, 4.0, 0.1, 1.8), P("target_atr", 1.5, 8.0, 0.1, 3.2)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& f = ctx.bitget_features;
        const double px = su::price(ctx);
        // The channel the PREVIOUS 20 bars made. AltTech's donchian_high_20 is
        // MAX(high, 20) including the bar that just closed, so a break of it
        // needed price to clear the breakout bar's own high after the close:
        // 270 such bars in seven days against 619 breaks of the prior channel.
        const double hi = f.get("donchian_high_20_prev"), lo = f.get("donchian_low_20_prev");
        const double a = su::atr(ctx);
        if (px <= 0 || hi <= 0 || lo <= 0 || a <= 0) return std::nullopt;
        // SETUP (the definition): price beyond the prior 20-bar channel by more
        // than quote noise. The hand-set 2 bps buffer is replaced by half the
        // observed spread (strat_util.hpp). Everything the detector used to
        // demand on top -- an allowed 1m regime, 1.2 ATR of room to the next
        // zone, expansion or volume -- is a judgement about whether the break
        // is worth trading, and is recorded below instead.
        const double buf = px * su::noise_bound_bps(ctx) / 10'000.0;
        Side side = Side::unspecified;
        if (px > hi + buf) side = Side::long_;
        else if (px < lo - buf) side = Side::short_;
        if (side == Side::unspecified) return std::nullopt;

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 1.8), p.get("target_atr", 3.2));
        s.holding_class = HoldingClass::intraday;
        s.expected_horizon_ms = 5LL * 3600 * 1000;
        const double cv_break = f.get("momentum_direction_agreement_15m", 0);
        s.cross_venue_confirmation = std::clamp(std::abs(cv_break), 0.0, 1.0);
        const double atr_exp = f.get("atr_expansion", 1.0);
        const double vol_z = ctx.microstructure.get("volume_surge", 1.0);
        // Diagnostic only (the static score ranks and gates nothing).
        s.raw_setup_score = std::clamp(0.45 + su::conviction(atr_exp - 1.0, 1.0) * 0.3 +
                                       su::conviction(vol_z, 3.0) * 0.25, 0.0, 1.0);
        s.reason_codes = {"donchian_breakout"};
        su::put_context(s, ctx);
        su::put(s, "break_distance_atr", side == Side::long_ ? (px - hi) / a : (lo - px) / a);
        su::put(s, "channel_width_atr", (hi - lo) / a);
        su::put_room(s, ctx, side, px, a);
        return s;
    }

private:
    StrategyMeta m_;
};

// -------------------------------------------------------------------------
// 12.6 ema_compression_expansion  (SHADOW)
// -------------------------------------------------------------------------
class EmaCompressionExpansion final : public IStrategy {
public:
    EmaCompressionExpansion() {
        m_.strategy_id = "ema_compression_expansion";
        m_.family = Family::breakout;
        m_.holding_classes = {HoldingClass::intraday};
        m_.required_features = {"ema_compression", "atr_14", "ema_20", "ema_50"};
        m_.default_lifecycle = Lifecycle::shadow;
        m_.explanation_template = "15m EMA compression resolving into expansion";
        // compression_max defines the allowed EMA20/EMA50 distance as a share
        // of price. The measured compression also travels as a descriptor.
        m_.parameter_schema = {P("compression_max", 0.001, 0.02, 0.0005, 0.004),
                               P("stop_atr", 1.0, 4.0, 0.1, 1.6), P("target_atr", 1.5, 8.0, 0.1, 3.0)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& f = ctx.bitget_features;
        const double px = su::price(ctx);
        const double comp = f.get("ema_compression");   // |ema20-ema50|/price recently
        const double ema20 = f.get("ema_20"), ema50 = f.get("ema_50");
        const double a = su::atr(ctx);
        const double atr_exp = f.get("atr_expansion", 1.0);
        const double vol_z = ctx.microstructure.get("volume_surge", 1.0);
        if (px <= 0 || ema20 <= 0 || ema50 <= 0 || a <= 0) return std::nullopt;
        // SETUP: a prior compression that is now resolving. Both halves are the
        // definition: compressed EMAs, and measured expansion (ATR ratio >= 1.1
        // or volume at or above its norm). An absent volume_surge defaulted to
        // 1.0 and passed unseen, so both must be measured.
        if (comp > p.get("compression_max", 0.004)) return std::nullopt;
        const bool expanding = f.has("atr_expansion") && atr_exp >= 1.1;
        const bool volume = ctx.microstructure.has("volume_surge") && vol_z >= 1.0;
        if (!expanding && !volume) return std::nullopt;
        // Direction from the structure that resolved: EMA stack and price on
        // its side. The venue agreement it also required is a confirmation,
        // recorded as a descriptor, not a condition.
        Side side = Side::unspecified;
        if (ema20 > ema50 && px > ema20) side = Side::long_;
        else if (ema20 < ema50 && px < ema20) side = Side::short_;
        if (side == Side::unspecified) return std::nullopt;

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 1.6), p.get("target_atr", 3.0));
        s.holding_class = HoldingClass::intraday;
        s.expected_horizon_ms = 4LL * 3600 * 1000;
        const double cv_agree = f.get("momentum_direction_agreement_15m", 0.0);
        s.cross_venue_confirmation = std::clamp(std::abs(cv_agree), 0.0, 1.0);
        s.raw_setup_score = std::clamp(0.4 + su::conviction(atr_exp - 1.0, 1.0) * 0.35 +
                                       su::conviction(vol_z, 3.0) * 0.25, 0.0, 1.0);
        s.reason_codes = {"ema_compression_prior", expanding ? "atr_expansion" : "volume_expansion"};
        su::put_context(s, ctx);
        su::put(s, "ema_compression", comp);
        su::put(s, "ema_spread_atr", (ema20 - ema50) / a);
        su::put(s, "dist_ema20_atr", (px - ema20) / a);
        return s;
    }

private:
    StrategyMeta m_;
};

} // namespace

ALTSTRATEGY_V2_REGISTER_STRATEGY(DonchianBreakoutContinuation)
ALTSTRATEGY_V2_REGISTER_STRATEGY(EmaCompressionExpansion)

} // namespace altstrategy::engine
