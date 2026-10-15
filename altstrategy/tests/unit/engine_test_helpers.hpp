#pragma once

// Shared builders for engine unit tests: synthetic DecisionContexts over the
// canonical feature vocabulary (docs/DECISION_CONTEXT.md) plus price paths.

#include "altstrategy/engine/decision_types.hpp"
#include "altstrategy/engine/outcomes.hpp"
#include "altstrategy/engine/strategy_engine.hpp"

namespace altstrategy::engine::test {

// A calibrated econ estimate as AltML schema 2 publishes it: one horizon, the
// three class probabilities and the expected net per side (cost included).
inline MarketForecastView econ_forecast(int32_t horizon, double net_long_bps, double net_short_bps,
                                        int64_t bar_open_ms = 960'000) {
    MarketForecastView f;
    f.present = true;
    f.econ = true;
    f.horizon_minutes = horizon;
    f.objective = "multiclass:econ_class_" + std::string(horizon == 15 ? "15m" : horizon == 60 ? "1h" : "4h");
    f.p_long = 0.40;
    f.p_neutral = 0.45;
    f.p_short = 0.15;
    f.expected_net_long_bps = net_long_bps;
    f.expected_net_short_bps = net_short_bps;
    f.round_trip_cost_bps = 16.0;
    f.model_id = "econ-test-" + std::to_string(horizon);
    f.bar_open_time_ms = bar_open_ms;
    f.prediction_schema_version = 3;
    f.calibrator = "temperature-1";
    f.recipe_version = "recipe-1";
    f.cost_model_version = "cost-v2";
    f.fee_bps = 12.0;
    f.spread_bps = 2.0;
    f.impact_bps = 2.0;
    f.spread_observed = true;
    f.policy_margin_bps = 0.0;   // a validated margin of zero: any E > 0 is POSITIVE
    f.policy_version = "policy-2";
    return f;
}

// A fully-valid four-venue context priced at `px`, TREND_UP, with the common
// Bitget indicator features populated so the trend strategies are eligible.
inline DecisionContext make_ctx(double px = 100.0, const std::string& regime = "TREND_UP") {
    DecisionContext c;
    c.context_id = "ctx-1";
    c.canonical_pair_id = 4200;
    c.bitget_instrument_id = 4200;
    c.symbol = "TESTUSDT";
    c.event_time_ms = 1'000'000;
    c.available_at_ms = 1'000'050;
    c.expires_at_ms = 1'000'000 + 60LL * 60 * 1000;   // valid window comfortably covers tests
    c.context_revision = 7;
    c.global_universe_status = "ELIGIBLE_ALL_VENUES";
    c.data_quality = ContextQuality::strict_4_of_4;
    c.venue_mask = 0b1111;
    c.feature_manifest_hash = "manifesthash-abc";
    c.mark_price = px;
    c.fees_bps = 4.0;
    c.spread_bps = 2.0;
    c.top_depth_usdt = 120000;
    c.liquidity_rank = 1.0;   // AltTech ordinal 1..4: Bitget the tightest book
    c.expected_slippage_bps = 1.5;
    c.funding_rate = 0.0001;

    const double atr = px * 0.01;
    c.bitget_features.valid = true;
    c.bitget_features.bar_open_time_ms = 999'000;
    c.bitget_features.values = {
        {"close", px},   {"open", px * 0.999}, {"high", px * 1.002}, {"low", px * 0.998},
        {"ema_20", px * 0.99}, {"ema_50", px * 0.97}, {"ema_200", px * 0.9}, {"rsi_14", 58},
        {"atr_14", atr}, {"adx_14", 28}, {"bb_width_20", 0.02}, {"volume_zscore_20", 1.2},
        {"vwap", px * 0.985}, {"vwap_dev", 15}, {"mom_1m", 0.001}, {"mom_5m", 0.003},
        {"mom_15m", 0.006}, {"donchian_high_20", px * 0.999}, {"donchian_low_20", px * 0.95},
        // The channel as the previous bar left it, and the session VWAP: what
        // the breakout and VWAP strategies read (the assembler derives both).
        {"donchian_high_20_prev", px * 0.999}, {"donchian_low_20_prev", px * 0.95},
        {"session_vwap", px * 0.985}, {"session_vwap_dev_bps", 15},
        {"ema_compression", 0.003}, {"atr_expansion", 1.3},
    };
    c.cross_venue_features.valid = true;
    c.cross_venue_features.values = {
        {"momentum_agreement", 0.4}, {"lead_confidence", 0.7}, {"bitget_lag", 1.0},
        {"dislocation", 6.0}, {"funding_divergence", 0.001}, {"oi_growth", 0.12},
        {"breakout_agreement", 0.3}, {"atr_14", atr},
        // The engine's signed agreements (context_assembler): direction x share.
        {"momentum_direction_agreement", 0.4}, {"flow_direction_agreement", 0.3},
        {"venue_price_dispersion_bps", 1.0}, {"consensus_flow", 1000.0},
    };
    c.microstructure.valid = true;
    c.microstructure.values = {
        {"trade_imbalance", 0.3}, {"cvd_delta", 1.5}, {"burst_score", 0.6},
        {"turnover_acceleration", 1.0}, {"volume_zscore", 2.5}, {"trade_intensity", 8.0},
    };
    c.bitget_regime.regime = regime;
    c.bitget_regime.confidence = 0.7;
    c.global_regime.regime = regime;
    c.global_regime.confidence = 0.72;

    c.market_forecast.present = true;
    c.market_forecast.abstain = false;
    c.market_forecast.p_up = 0.58;
    c.market_forecast.p_down = 0.25;
    c.market_forecast.expected_return = 0.004;
    // Return fractions, as AltML sends them (this fixture used to set 90 and
    // 40 and call them bps -- the very misreading QUALITY-01 is about).
    c.market_forecast.expected_mfe_long_fraction = 0.0090;
    c.market_forecast.expected_mae_long_fraction = -0.0040;
    // Conviction, not uncertainty: 0.58 up vs 0.25 down is |0.58-0.5|*2 = 0.16.
    c.market_forecast.conviction = 0.16;
    c.market_forecast.quality_status = "VALID";
    c.market_forecast.model_id = "test-model";
    c.market_forecast.prediction_schema_version = 2;

    // Bitget's own 15m / 1h regime (regime_module) and the venue agreement on
    // the decision grid -- what the trend-family setups read since they
    // stopped reading the 1m regime and the 1m consensus.
    for (const int32_t tf : {15, 60}) {
        RegimeView v;
        v.regime = regime;
        v.confidence = 0.7;
        v.bar_open_time_ms = 999'000 - tf * 60'000;
        c.bitget_htf_regime[tf] = v;
    }
    c.bitget_features.values["momentum_direction_agreement_15m"] =
        regime == "TREND_DOWN" ? -0.5 : 0.5;
    // Econ estimates for every horizon: by default a long clears the cost
    // (+8 bps net) and a short does not (-40 bps net). Tests that need another
    // verdict overwrite them; tests that need "no model" erase them.
    for (const int32_t h : {15, 60, 240}) c.forecasts[h] = econ_forecast(h, 8.0, -40.0);
    return c;
}

// A monotone up path from px0 that reaches `hi` then optionally dips.
inline std::vector<PathBar> up_path(int64_t start_ms, double px0, double hi, int n = 20) {
    std::vector<PathBar> bars;
    for (int i = 0; i < n; ++i) {
        const double frac = static_cast<double>(i) / (n - 1);
        const double mid = px0 + (hi - px0) * frac;
        PathBar b;
        b.open_ms = start_ms + i * 60'000;
        b.open = mid;
        b.high = mid * 1.001;
        b.low = mid * 0.999;
        b.close = mid;
        bars.push_back(b);
    }
    return bars;
}

} // namespace altstrategy::engine::test
