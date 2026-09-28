// VWAP-family (§12.8 vwap_reclaim_continuation) and CROSS_VENUE-family
// (§12.7 cross_venue_lead_lag_follow). Both new, seeded SHADOW.

#include "altstrategy/engine/strat_util.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {
namespace {

StrategyParam P(const std::string& n, double lo, double hi, double step, double def) {
    return StrategyParam{n, "double", lo, hi, step, def};
}

// -------------------------------------------------------------------------
// 12.8 vwap_reclaim_continuation  (SHADOW)
// -------------------------------------------------------------------------
class VwapReclaimContinuation final : public IStrategy {
public:
    VwapReclaimContinuation() {
        m_.strategy_id = "vwap_reclaim_continuation";
        m_.family = Family::vwap;
        m_.holding_classes = {HoldingClass::intraday};
        m_.required_features = {"session_vwap", "session_vwap_dev_bps", "atr_14"};
        m_.default_lifecycle = Lifecycle::shadow;
        m_.explanation_template = "session VWAP lost within five bars, then reclaimed";
        m_.parameter_schema = {P("stop_atr", 1.0, 4.0, 0.1, 1.5), P("target_atr", 1.5, 8.0, 0.1, 2.8)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& f = ctx.bitget_features;
        // The SESSION VWAP (UTC day, see engine_io::SessionVwap). This read
        // `vwap`, the VWAP of the last 1-minute trade window -- the price of the
        // minute just gone -- and price changed sides of it on 89.9% of
        // decision bars, so "lost then reclaimed" held almost always.
        const double px = su::price(ctx), vwap = f.get("session_vwap");
        const double vwap_dev = f.get("session_vwap_dev_bps"); // (px-vwap)/vwap, bps
        const double a = su::atr(ctx);
        const double lost = f.get("session_vwap_lost_recently", 0);  // side changed within 5 bars
        if (px <= 0 || vwap <= 0 || a <= 0) return std::nullopt;
        // SETUP: the session VWAP was lost within five bars and price now
        // stands beyond it by more than quote noise (half the observed spread;
        // it was a hand-set 3 bps). The CVD sign, the 1m global regime ("not a
        // downtrend") and one ATR of room to the next zone were conditions on
        // whether to TRADE the reclaim; they are recorded, not required.
        const double hold = su::noise_bound_bps(ctx);

        Side side = Side::unspecified;
        if (lost > 0 && px > vwap && vwap_dev > hold) side = Side::long_;
        else if (lost > 0 && px < vwap && vwap_dev < -hold) side = Side::short_;
        if (side == Side::unspecified) return std::nullopt;
        const double cvd = ctx.microstructure.get("cvd_delta", 0.0);

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 1.5), p.get("target_atr", 2.8));
        s.holding_class = HoldingClass::intraday;
        s.expected_horizon_ms = 3LL * 3600 * 1000;
        s.cross_venue_confirmation = std::clamp(std::abs(ctx.cross_venue_features.get("momentum_agreement", 0)), 0.0, 1.0);
        s.raw_setup_score = std::clamp(0.4 + su::conviction(std::abs(vwap_dev), 10.0) * 0.3 +
                                       su::conviction(cvd, 1.0) * 0.3, 0.0, 1.0);
        s.reason_codes = {"vwap_reclaim"};
        su::put_context(s, ctx);
        su::put(s, "session_vwap_dev_bps", vwap_dev);
        su::put(s, "session_vwap_dev_atr", (px - vwap) / a);
        su::put_room(s, ctx, side, px, a);
        return s;
    }

private:
    StrategyMeta m_;
};

// -------------------------------------------------------------------------
// 12.7 cross_venue_lead_lag_follow  (SHADOW). Never hardcodes a fixed leader.
// -------------------------------------------------------------------------
class CrossVenueLeadLagFollow final : public IStrategy {
public:
    CrossVenueLeadLagFollow() {
        m_.strategy_id = "cross_venue_lead_lag_follow";
        m_.family = Family::cross_venue;
        m_.holding_classes = {HoldingClass::scalp, HoldingClass::intraday};
        m_.required_features = {"bitget_lead_score", "bitget_lag_ms", "bitget_mid_deviation_bps", "atr_14"};
        m_.default_lifecycle = Lifecycle::shadow;
        m_.max_candidate_freq_ms = 30'000;
        m_.explanation_template = "Bitget trails the consensus and stands off it: expect catch-up";
        m_.parameter_schema = {P("stop_atr", 0.8, 3.0, 0.1, 1.2), P("target_atr", 1.0, 5.0, 0.1, 2.0)};
    }
    const StrategyMeta& meta() const override { return m_; }
    std::optional<RawSignal> evaluate(const DecisionContext& ctx, const ParamValues& p) const override {
        const auto& cv = ctx.cross_venue_features;
        const double px = su::price(ctx), a = su::atr(ctx);
        // AltTech's `bitget_lead_score`, signed on [-5, +5]: POSITIVE means Bitget
        // leads the consensus, NEGATIVE means it follows. This was read under the
        // name `lead_confidence` and tested as `>= 0.55`, which asked for Bitget
        // to be LEADING -- while the next line asks for it to be LAGGING. The two
        // conditions cannot both hold, and measured over seven days on VPS1 they
        // never did: 0 contexts out of 10 137 passed both, so the strategy could
        // not fire once. The hypothesis is "follow the venue that leads while
        // Bitget trails", so the conviction required here is the depth of
        // Bitget's TRAILING.
        const double bitget_lead = cv.get("bitget_lead_score");
        const double lag_conviction = -bitget_lead;
        const double lag = cv.get("bitget_lag_ms");          // >0 => bitget lags consensus
        // Bitget's OWN signed gap to the consensus. This read `dislocation_bps`
        // -- AltTech's max_venue_price_deviation_bps, the largest venue gap as
        // a MAGNITUDE (never negative over 14 days) -- as if it were signed:
        // `disloc < 0` (Bitget below, catch up long) could not hold, and every
        // dislocation of any venue became a short on Bitget (165 shorts, 0 longs).
        const double disloc_bps = cv.get("bitget_mid_deviation_bps");
        if (px <= 0 || a <= 0 || !std::isfinite(disloc_bps)) return std::nullopt;
        // SETUP: Bitget measurably trails the consensus and stands off it.
        //  * trails: the lead score is a correlation-weighted mean lag in
        //    SAMPLES of AltTech's 1m grid (-5..+5); Bitget's is negative in 98 %
        //    of contexts, so a sign is a state, not an event. At least one full
        //    sample behind is the measurement's own resolution -- a smaller
        //    lag is indistinguishable from none -- and holds in ~1 % of
        //    contexts. The lag in ms must agree.
        //  * stands off: a two-dispersion outlier from the consensus
        //    (strat_util.hpp::dislocated), no longer a hand-set 4 bps.
        // The 0.55 floor it replaces sat on the same score with no stated
        // origin; the net-flow sign it also required is recorded below.
        if (!(bitget_lead <= -1.0) || !(lag > 0)) return std::nullopt;
        if (!su::dislocated(ctx, disloc_bps)) return std::nullopt;

        // Follow the consensus: if Bitget is BELOW consensus (disloc<0) expect catch-up UP.
        const Side side = disloc_bps < 0 ? Side::long_ : Side::short_;

        RawSignal s;
        s.side = side;
        su::geo_from_atr(s, side, px, a, p.get("stop_atr", 1.2), p.get("target_atr", 2.0));
        s.holding_class = HoldingClass::scalp;
        s.expected_horizon_ms = 45LL * 60 * 1000;
        s.cross_venue_confirmation = std::clamp(lag_conviction, 0.0, 1.0);
        s.raw_setup_score = std::clamp(0.4 + std::clamp(lag_conviction, 0.0, 1.0) * 0.35 +
                                       su::conviction(std::abs(disloc_bps), 12.0) * 0.25, 0.0, 1.0);
        s.reason_codes = {"bitget_lags_consensus", "dislocation"};
        su::put_context(s, ctx);
        su::put(s, "bitget_lead_score", bitget_lead);
        su::put(s, "bitget_lag_ms", lag);
        su::put(s, "bitget_mid_deviation_bps", disloc_bps);
        su::put_if(s, cv, "consensus_flow");
        return s;
    }

private:
    StrategyMeta m_;
};

} // namespace

ALTSTRATEGY_V2_REGISTER_STRATEGY(VwapReclaimContinuation)
ALTSTRATEGY_V2_REGISTER_STRATEGY(CrossVenueLeadLagFollow)

} // namespace altstrategy::engine
