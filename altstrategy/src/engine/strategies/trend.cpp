// TREND-family strategies (§12.1, §12.2, §12.3, §12.4).
//
// momentum_continuation and trend_pullback preserve the existing production
// edge, re-expressed over the four-venue DecisionContext (global regime,
// cross-venue momentum agreement, ML direction/net return, projected structure,
// liquidity). multi_timeframe_momentum_alignment and volume_weighted_momentum_
// burst are new.

#include "altstrategy/engine/strat_util.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {
namespace {

StrategyParam P(const std::string& n, double lo, double hi, double step, double def) {
    return StrategyParam{n, "double", lo, hi, step, def};
}

// -------------------------------------------------------------------------
// 12.1 momentum_continuation  (ACTIVE)
// -------------------------------------------------------------------------
class MomentumContinuation final : public IStrategy {
public:
    MomentumContinuation() {
        m_.strategy_id = "momentum_continuation";
        m_.family = Family::trend;
        m_.holding_classes = {HoldingClass::intraday};
        // The trend is read from Bitget's own 1h regime (see evaluate()). The
        // LOW_LIQUIDITY refusal it kept was an execution judgement (VPS2's),
        // read from a 1m regime; liquidity is recorded, not gated.
        m_.required_features = {"ema_20", "ema_50", "atr_14", "adx_14"};
        m_.required_quality = ContextQuality::degraded_2_of_4;
        m_.default_lifecycle = Lifecycle::active;
        m_.explanation_template = "Momentum continuation aligned with the Bitget 1h regime";
        m_.parameter_schema = {P("adx_min", 12, 40, 1, 20), P("stop_atr", 1.0, 4.0, 0.1, 2.0),
                               P("target_atr", 1.5, 8.0, 0.1, 3.0)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& f = ctx.bitget_features;
        const double px = su::price(ctx);
        const double ema20 = f.get("ema_20"), ema50 = f.get("ema_50");
        const double adx = f.get("adx_14");
        if (px <= 0 || ema20 <= 0 || ema50 <= 0) return std::nullopt;
        // No 15m ADX floor any more: the trend statement is the 1h regime
        // (AltTech requires ADX >= 25 on 1h for TREND_UP/DOWN), and a second,
        // hand-set strength floor on the 15m grid was a confirmation filter the
        // model can apply better -- ADX is one of its inputs. `adx_min` now only
        // shapes the diagnostic raw score.

        // SETUP: price on the trend side of a stacked 15m EMA pair, in the
        // direction of Bitget's 1h regime. The 1h regime is the trend
        // statement; the 1m global regime it used to read cannot make one
        // about a 15m continuation. Whether the continuation pays is the
        // economic policy's question, not a threshold here.
        const auto h1 = ctx.bitget_htf_regime.find(60);
        if (h1 == ctx.bitget_htf_regime.end()) return std::nullopt;   // no trend statement, no setup
        const std::string& hr = h1->second.regime;
        const double cv_mom = f.get("momentum_direction_agreement_15m", 0);
        Side side = Side::unspecified;
        if (ema20 > ema50 && px > ema20 && su::regime_is_trend_up(hr)) side = Side::long_;
        else if (ema20 < ema50 && px < ema20 && su::regime_is_trend_down(hr)) side = Side::short_;
        if (side == Side::unspecified) return std::nullopt;

        // No model gate here. Whether this setup pays is the economic
        // policy's question (economic_policy.hpp), asked once, of a calibrated
        // estimate for the setup's horizon. A per-strategy veto on a raw
        // probability (the 0.55 / 0.6 cuts that stood here) was a hand-set
        // boundary on a number that meant something else.

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, su::atr(ctx), p.get("stop_atr", 2.0), p.get("target_atr", 3.0));
        s.holding_class = HoldingClass::intraday;
        s.expected_horizon_ms = 4LL * 3600 * 1000;
        s.cross_venue_confirmation = std::clamp(std::abs(cv_mom), 0.0, 1.0);
        s.raw_setup_score = std::clamp(0.4 + su::conviction(adx - p.get("adx_min", 20), 25) * 0.4 +
                                       std::abs(cv_mom) * 0.2, 0.0, 1.0);
        s.reason_codes = {"ema_stack_aligned",
                          side == Side::long_ ? "h1_regime_trend_up" : "h1_regime_trend_down"};
        if (ctx.regime_divergence) s.warnings.push_back("BITGET_GLOBAL_REGIME_DIVERGENCE");
        su::put_context(s, ctx);
        su::put(s, "adx_14_15m", adx);
        su::put(s, "dist_ema20_atr", su::atr(ctx) > 0 ? (px - ema20) / su::atr(ctx) : NAN);
        su::put(s, "h1_regime_confidence", h1->second.confidence);
        return s;
    }

private:
    StrategyMeta m_;
};

// -------------------------------------------------------------------------
// 12.2 trend_pullback  (ACTIVE)
// -------------------------------------------------------------------------
class TrendPullback final : public IStrategy {
public:
    TrendPullback() {
        m_.strategy_id = "trend_pullback";
        m_.family = Family::trend;
        m_.holding_classes = {HoldingClass::intraday};
        // TREND_PULLBACK_SETUP: Bitget 1h regime + a 15m pullback against it.
        //
        // It read the 1m global regime (TREND_UP = (EMA20-EMA200)/ATR > 1 on
        // 1m bars) against a pullback measured on 15m bars; over seven days the
        // two never coincided -- 634 such pullbacks, every one in a 1m
        // TREND_DOWN or BREAKOUT -- so the only ACTIVE strategy was dead by
        // construction. The trend is now the 1h regime AltTech computes for
        // Bitget. The depth band (0.8-3.0 ATR) and the RSI guard are gone: they
        // were hand-set boundaries, and the depth is an input the model sees
        // (distance to EMA20 in ATR on the 15m grid). A shallow-band variant
        // looked good in-sample; nothing here promotes it. The detector stays
        // CANDIDATE until a fresh out-of-sample check says otherwise.
        m_.required_features = {"ema_20", "ema_50", "atr_14"};
        m_.default_lifecycle = Lifecycle::candidate;
        m_.explanation_template = "15m pullback against the Bitget 1h trend";
        m_.parameter_schema = {P("stop_atr", 1.0, 4.0, 0.1, 1.8), P("target_atr", 1.5, 8.0, 0.1, 3.5)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& f = ctx.bitget_features;
        const double px = su::price(ctx), ema20 = f.get("ema_20"), ema50 = f.get("ema_50");
        const double a = su::atr(ctx);
        if (px <= 0 || ema20 <= 0 || ema50 <= 0 || a <= 0) return std::nullopt;
        const auto h1 = ctx.bitget_htf_regime.find(60);
        if (h1 == ctx.bitget_htf_regime.end()) return std::nullopt;   // no 1h trend statement
        const std::string& hr = h1->second.regime;
        const double cv_mom = f.get("momentum_direction_agreement_15m", 0);

        Side side = Side::unspecified;
        double depth_atr = 0;
        if (su::regime_is_trend_up(hr) && ema20 > ema50 && px < ema20) {
            depth_atr = (ema20 - px) / a;
            side = Side::long_;
        } else if (su::regime_is_trend_down(hr) && ema20 < ema50 && px > ema20) {
            depth_atr = (px - ema20) / a;
            side = Side::short_;
        }
        if (side == Side::unspecified) return std::nullopt;

        RawSignal s;
        s.side = side;
        // Entry at current pullback price; structure support/resistance shapes stop.
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 1.8), p.get("target_atr", 3.5));
        s.holding_class = HoldingClass::intraday;
        s.expected_horizon_ms = 4LL * 3600 * 1000;
        s.cross_venue_confirmation = std::clamp(std::abs(cv_mom), 0.0, 1.0);
        // Diagnostic only (the static score no longer ranks anything).
        s.raw_setup_score = std::clamp(0.45 + su::conviction(depth_atr, 3.0) * 0.35 +
                                       std::abs(cv_mom) * 0.2, 0.0, 1.0);
        s.reason_codes = {"trend_pullback_setup", side == Side::long_ ? "h1_trend_up" : "h1_trend_down"};
        su::put_context(s, ctx);
        su::put(s, "pullback_depth_atr", depth_atr);
        su::put(s, "h1_regime_confidence", h1->second.confidence);
        return s;
    }

private:
    StrategyMeta m_;
};

// -------------------------------------------------------------------------
// 12.3 multi_timeframe_momentum_alignment  (CANDIDATE)
// -------------------------------------------------------------------------
class MtfMomentumAlignment final : public IStrategy {
public:
    MtfMomentumAlignment() {
        m_.strategy_id = "multi_timeframe_momentum_alignment";
        m_.family = Family::trend;
        m_.holding_classes = {HoldingClass::intraday};
        m_.required_features = {"mom_short", "ema_20", "atr_14"};
        m_.default_lifecycle = Lifecycle::candidate;
        m_.explanation_template =
            "15m + 30m + 1h momentum aligned with the Bitget 15m and 1h regimes";
        m_.parameter_schema = {P("stop_atr", 1.0, 4.0, 0.1, 1.6), P("target_atr", 1.5, 8.0, 0.1, 3.0)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& f = ctx.bitget_features;
        const double px = su::price(ctx);
        // Momentum on THREE GRIDS, which is what this strategy is named after.
        //
        // It read mom_short/mom_mid/mom_long from ctx.bitget_features -- all
        // three from the SAME execution vector, where they are ROC over 5, 14 and
        // 20 bars. On the 15m grid that is 75, 210 and 300 minutes of one
        // timeframe, not "1m/5m/15m" as the explanation claimed, and the higher
        // grids the assembler admits (30m, 1h, 4h, 1d) were read by nothing.
        // Ablation showed it directly: keeping only the 30m, only the 1h or only
        // the 1d grid produced byte-identical results (6586 trades, -4.01 bps in
        // every arm), because no decision depended on any of them.
        //
        // A grid with no valid closed observation is ABSENT, and absent means no
        // candidate -- an alignment across three grids cannot be asserted from
        // two.
        const auto grid_mom = [&ctx](int32_t minutes) -> std::optional<double> {
            const auto it = ctx.context_features.find(minutes);
            if (it == ctx.context_features.end() || !it->second.valid) return std::nullopt;
            const auto& v = it->second;
            if (!v.has("roc_14")) return std::nullopt;
            const double m = v.get("roc_14");
            if (!std::isfinite(m)) return std::nullopt;
            return m;
        };
        const double m_exec = f.get("mom_short");
        const auto m30 = grid_mom(30);
        const auto m60 = grid_mom(60);
        if (!m30 || !m60) return std::nullopt;
        // The regime on the grids the momentum is read on: Bitget's 15m and 1h
        // regimes. It read the 1m venue regime and the 1m global regime -- two
        // statements about the last minutes -- as confirmation of a three-grid
        // alignment. The session-VWAP side and the venue agreement it also
        // required are confirmations, recorded below, not conditions.
        const double cv_mom = f.get("momentum_direction_agreement_15m", 0.0);
        const auto r15 = ctx.bitget_htf_regime.find(15);
        const auto r60 = ctx.bitget_htf_regime.find(60);
        if (r15 == ctx.bitget_htf_regime.end() || r60 == ctx.bitget_htf_regime.end()) return std::nullopt;
        const std::string& br = r15->second.regime, &gr = r60->second.regime;
        if (px <= 0) return std::nullopt;

        Side side = Side::unspecified;
        if (m_exec > 0 && *m30 > 0 && *m60 > 0 && su::regime_is_trend_up(br) && su::regime_is_trend_up(gr))
            side = Side::long_;
        else if (m_exec < 0 && *m30 < 0 && *m60 < 0 && su::regime_is_trend_down(br) &&
                 su::regime_is_trend_down(gr))
            side = Side::short_;
        if (side == Side::unspecified) return std::nullopt;

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, su::atr(ctx), p.get("stop_atr", 1.6), p.get("target_atr", 3.0));
        s.holding_class = HoldingClass::intraday;
        s.expected_horizon_ms = 3LL * 3600 * 1000;
        s.cross_venue_confirmation = std::clamp(std::abs(cv_mom), 0.0, 1.0);
        const double align = (std::abs(m_exec) + std::abs(*m30) + std::abs(*m60)) / 3.0;
        s.raw_setup_score = std::clamp(0.5 + su::conviction(align, 0.01) * 0.3 + std::abs(cv_mom) * 0.2,
                                       0.0, 1.0);
        s.reason_codes = {"mtf_momentum_aligned"};
        su::put_context(s, ctx);
        su::put(s, "roc_14_15m", m_exec);
        su::put(s, "roc_14_30m", *m30);
        su::put(s, "roc_14_1h", *m60);
        if (f.has("session_vwap") && f.get("session_vwap") > 0)
            su::put(s, "session_vwap_dev_bps", (px / f.get("session_vwap") - 1.0) * 10'000.0);
        return s;
    }

private:
    StrategyMeta m_;
};

// -------------------------------------------------------------------------
// 12.4 volume_weighted_momentum_burst  (CANDIDATE)
// -------------------------------------------------------------------------
class VolumeWeightedMomentumBurst final : public IStrategy {
public:
    VolumeWeightedMomentumBurst() {
        m_.strategy_id = "volume_weighted_momentum_burst";
        m_.family = Family::trend;
        m_.holding_classes = {HoldingClass::scalp, HoldingClass::intraday};
        m_.required_features = {"mom_short", "atr_14"};
        m_.default_lifecycle = Lifecycle::candidate;
        m_.max_candidate_freq_ms = 60'000;
        m_.explanation_template = "Volume/CVD-confirmed impulse burst";
        // vol_z_min is DEFINITIONAL: a burst is volume at twice its trailing
        // norm or more (the conventional two-fold anomaly); the measured surge
        // travels as a descriptor.
        m_.parameter_schema = {P("vol_z_min", 1.0, 5.0, 0.1, 2.0), P("stop_atr", 0.8, 3.0, 0.1, 1.4),
                               P("target_atr", 1.0, 6.0, 0.1, 2.2)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& f = ctx.bitget_features;
        const auto& mi = ctx.microstructure;
        const double px = su::price(ctx), m1 = f.get("mom_short");
        const double vol_z = mi.get("volume_surge", 1.0);
        const double cvd = mi.get("cvd_delta"), imb = mi.get("trade_imbalance");
        const double turn_acc = mi.get("turnover_acceleration");
        // The venue agreement is a confirmation of the burst, not part of it:
        // recorded, not required.
        const double cv_mom = ctx.cross_venue_features.get("momentum_direction_agreement", 0.0);
        if (px <= 0) return std::nullopt;
        // SETUP: a volume anomaly whose executed flow (CVD, trade imbalance)
        // points the same way as the move. Both are measured, never defaulted.
        if (!mi.has("volume_surge") || !mi.has("cvd_delta") || !mi.has("trade_imbalance")) return std::nullopt;
        if (vol_z < p.get("vol_z_min", 2.0)) return std::nullopt;

        Side side = Side::unspecified;
        if (m1 > 0 && cvd > 0 && imb > 0) side = Side::long_;
        else if (m1 < 0 && cvd < 0 && imb < 0) side = Side::short_;
        if (side == Side::unspecified) return std::nullopt;

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, su::atr(ctx), p.get("stop_atr", 1.4), p.get("target_atr", 2.2));
        s.holding_class = vol_z > 3.0 ? HoldingClass::scalp : HoldingClass::intraday;
        s.expected_horizon_ms = 90LL * 60 * 1000;
        s.cross_venue_confirmation = std::clamp(std::abs(cv_mom), 0.0, 1.0);
        s.raw_setup_score = std::clamp(0.4 + su::conviction(vol_z, 3.0) * 0.35 +
                                       su::conviction(turn_acc, 2.0) * 0.25, 0.0, 1.0);
        s.reason_codes = {"volume_burst", "cvd_confirms", "trade_imbalance_confirms"};
        su::put_context(s, ctx);
        su::put(s, "roc_5_15m", m1);
        su::put_if(s, mi, "turnover_acceleration");
        return s;
    }

private:
    StrategyMeta m_;
};

} // namespace

ALTSTRATEGY_V2_REGISTER_STRATEGY(MomentumContinuation)
ALTSTRATEGY_V2_REGISTER_STRATEGY(TrendPullback)
ALTSTRATEGY_V2_REGISTER_STRATEGY(MtfMomentumAlignment)
ALTSTRATEGY_V2_REGISTER_STRATEGY(VolumeWeightedMomentumBurst)

} // namespace altstrategy::engine
