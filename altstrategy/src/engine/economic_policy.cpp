#include "altstrategy/engine/economic_policy.hpp"
#include "altstrategy/engine/forecast_checks.hpp"

#include <cmath>

namespace altstrategy::engine {

int32_t evaluation_horizon_minutes(int64_t expected_horizon_ms) {
    constexpr int64_t kMin = 60'000;
    // A horizon return distribution is not a forecast of a different holding
    // period. Keep the genuine setup horizon; unsupported durations have no
    // compatible monetary estimate until a matching artifact is trained.
    for (const int32_t horizon : {15, 60, 240}) {
        if (expected_horizon_ms == static_cast<int64_t>(horizon) * kMin) return horizon;
    }
    return 0;
}

namespace {

using fc::incomplete;
using fc::shadow;
using fc::usable;
bool fresh(const MarketForecastView& f, int64_t now_ms, const EconomicPolicyConfig& cfg) {
    return fc::fresh(f, now_ms, cfg.max_forecast_age_ms);
}

} // namespace

EconomicDecision evaluate_economics(Side side, int64_t expected_horizon_ms,
                                    const DecisionContext& ctx, int64_t now_ms,
                                    const EconomicPolicyConfig& cfg) {
    return evaluate_economics_at(side, evaluation_horizon_minutes(expected_horizon_ms), ctx, now_ms,
                                 cfg);
}

EconomicDecision evaluate_economics_at(Side side, int32_t horizon_minutes,
                                       const DecisionContext& ctx, int64_t now_ms,
                                       const EconomicPolicyConfig& cfg) {
    EconomicDecision d;
    d.horizon_minutes = horizon_minutes;
    if (horizon_minutes <= 0) {
        d.state = EconState::model_unavailable;
        d.reason_code = "PAYOFF_HORIZON_UNSUPPORTED";
        return d;
    }
    const auto it = ctx.forecasts.find(d.horizon_minutes);
    if (it == ctx.forecasts.end() || !it->second.present) {
        d.state = EconState::model_unavailable;
        d.reason_code = "MODEL_UNAVAILABLE";
        return d;
    }
    const auto& f = it->second;
    d.model_id = f.model_id;
    d.forecast_bar_open_ms = f.bar_open_time_ms;
    if (f.abstain) {
        d.state = EconState::model_abstain;
        d.reason_code = "MODEL_ABSTAIN";
        return d;
    }
    if (!usable(f) || f.horizon_minutes != d.horizon_minutes ||
        (side != Side::long_ && side != Side::short_)) {
        d.state = EconState::model_unavailable;
        d.reason_code = "MODEL_INCOMPATIBLE";
        return d;
    }
    if (!fresh(f, now_ms, cfg)) {
        d.state = EconState::model_stale;
        d.reason_code = "MODEL_STALE";
        return d;
    }
    const bool is_long = side == Side::long_;
    d.p_side = is_long ? f.p_long : f.p_short;
    d.p_opposite = is_long ? f.p_short : f.p_long;
    d.p_neutral = f.p_neutral;
    d.expected_net_bps = is_long ? f.expected_net_long_bps : f.expected_net_short_bps;
    d.round_trip_cost_bps = f.round_trip_cost_bps;
    d.expected_gross_bps = d.expected_net_bps + d.round_trip_cost_bps;
    d.margin_bps = f.policy_margin_bps;
    if (shadow(f)) {
        d.state = EconState::model_shadow;
        d.reason_code = "MODEL_SHADOW";
        return d;
    }
    if (incomplete(f)) {
        d.state = EconState::no_data;
        d.reason_code = "NO_DATA";
        return d;
    }

    // Model agreement across horizons: a fresh, usable estimate on ANOTHER
    // horizon that says the opposite side clears its costs by its own margin
    // is a disagreement about direction, not noise to be averaged away.
    for (const auto& [h, g] : ctx.forecasts) {
        if (h == d.horizon_minutes || !g.present || g.abstain || !usable(g) || shadow(g) ||
            incomplete(g) || !fresh(g, now_ms, cfg)) {
            continue;
        }
        const double opposite_net = is_long ? g.expected_net_short_bps : g.expected_net_long_bps;
        if (opposite_net > g.policy_margin_bps.value_or(0.0) && opposite_net > 0.0) {
            d.state = EconState::horizon_conflict;
            d.reason_code = "HORIZON_CONFLICT";
            return d;
        }
    }
    d.state = classify_edge(d.expected_net_bps, d.round_trip_cost_bps, d.margin_bps);
    d.reason_code = to_string(d.state);
    return d;
}

EconState classify_edge(double expected_net_bps, double round_trip_cost_bps,
                        std::optional<double> margin_bps) {
    if (!(expected_net_bps > 0.0)) return EconState::no_edge;
    if (!margin_bps.has_value() || !std::isfinite(*margin_bps) || *margin_bps < 0.0 ||
        expected_net_bps <= *margin_bps) {
        return EconState::small_edge;
    }
    if (expected_net_bps >= *margin_bps + round_trip_cost_bps) return EconState::strong_edge;
    return EconState::positive_edge;
}

} // namespace altstrategy::engine
