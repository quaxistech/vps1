#include "altstrategy/engine/signal_predictions.hpp"

#include "altstrategy/engine/forecast_checks.hpp"
#include "altstrategy/engine/units.hpp"

#include <cmath>

namespace altstrategy::engine {

HorizonPrediction horizon_prediction(const DecisionContext& ctx, int32_t horizon_minutes, int64_t now_ms,
                                     int64_t max_forecast_age_ms) {
    HorizonPrediction h;
    h.horizon_minutes = horizon_minutes;
    const auto it = ctx.forecasts.find(horizon_minutes);
    if (it == ctx.forecasts.end() || !it->second.present) return h;   // MISSING
    const auto& f = it->second;
    h.model_id = f.model_id;
    h.model_version = f.model_version;
    h.objective = f.objective;
    h.calibrator = f.calibration_version.empty() ? f.calibrator : f.calibration_version;
    h.bar_open_time_ms = f.bar_open_time_ms;
    h.computed_at_ms = f.computed_at_ms;
    h.aligned_missing = f.aligned_missing;
    if (f.abstain) {
        h.state = PredictionState::abstain;
        return h;
    }
    if (!fc::usable(f) || f.horizon_minutes != horizon_minutes) {
        h.state = PredictionState::incompatible;
        return h;
    }
    h.role = fc::shadow(f) ? "SHADOW" : "ACTIVE";
    h.probability_calibrated = f.probability_calibrated;
    h.p_up = f.p_long;
    h.p_flat = f.p_neutral;
    h.p_down = f.p_short;
    h.expected_return_bps = units::fraction_to_bps(f.expected_return);
    // Excursions as magnitudes (>= 0). Schema 4 publishes them directly;
    // schema 3 as the long side's favourable (>= 0) and adverse (<= 0) moves.
    // Neither: no magnitude was estimated, which is not an estimate of 0.
    if (f.has_excursions) {
        h.has_excursions = true;
        h.expected_up_excursion_bps = f.expected_up_excursion_bps;
        h.expected_down_excursion_bps = f.expected_down_excursion_bps;
    } else if (f.has_side_bps) {
        h.has_excursions = true;
        h.expected_up_excursion_bps = std::max(f.favorable_long_bps, 0.0);
        h.expected_down_excursion_bps = std::max(-f.adverse_long_bps, 0.0);
    }
    h.cost_model_version = f.cost_model_version;
    h.cost_bps = f.round_trip_cost_bps;
    h.expected_net_long_bps = f.expected_net_long_bps;
    h.expected_net_short_bps = f.expected_net_short_bps;
    if (!fc::fresh(f, now_ms, max_forecast_age_ms)) h.state = PredictionState::stale;
    else if (fc::incomplete(f)) h.state = PredictionState::incomplete;
    else h.state = PredictionState::present;
    return h;
}

std::vector<HorizonPrediction> market_predictions(const DecisionContext& ctx, int64_t now_ms,
                                                  int64_t max_forecast_age_ms) {
    std::vector<HorizonPrediction> out;
    for (const int32_t h : kServedHorizons) out.push_back(horizon_prediction(ctx, h, now_ms, max_forecast_age_ms));
    return out;
}

const HorizonPrediction* evaluation_prediction(const StrategyCandidate& c) {
    for (const auto& h : c.predictions)
        if (h.horizon_minutes == c.econ.horizon_minutes) return &h;
    return nullptr;
}

const HorizonPrediction* geometry_prediction(const StrategyCandidate& c) {
    const auto* h = evaluation_prediction(c);
    return h != nullptr && h->state == PredictionState::present && h->role == "ACTIVE" ? h : nullptr;
}

std::optional<SideExcursions> side_excursions(const HorizonPrediction& h, Side side) {
    if (!h.has_excursions || (side != Side::long_ && side != Side::short_)) return std::nullopt;
    const double up = h.expected_up_excursion_bps, down = h.expected_down_excursion_bps;
    if (!std::isfinite(up) || !std::isfinite(down) || up < 0 || down < 0) return std::nullopt;
    return side == Side::long_ ? SideExcursions{up, down} : SideExcursions{down, up};
}

} // namespace altstrategy::engine
