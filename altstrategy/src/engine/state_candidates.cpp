#include "altstrategy/engine/state_candidates.hpp"

#include "altstrategy/engine/ids.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {

namespace {

HoldingClass holding_for(int32_t horizon_minutes) {
    const int64_t ms = static_cast<int64_t>(horizon_minutes) * 60'000;
    if (ms <= 30LL * 60 * 1000) return HoldingClass::scalp;
    if (ms <= 8LL * 3600 * 1000) return HoldingClass::intraday;
    return HoldingClass::extended_intraday;
}

} // namespace

std::vector<StrategyCandidate> state_candidates(
    const DecisionContext& ctx, int64_t now_ms, const EconomicPolicyConfig& econ,
    const StateCandidateConfig& cfg, const std::vector<StrategyCandidate>& setups,
    const std::function<bool(int32_t, Side)>& admit, int32_t only_horizon) {
    std::vector<StrategyCandidate> out;
    if (!cfg.enabled) return out;
    const double px = ctx.last_price();
    if (!(px > 0)) return out;
    for (const auto& [horizon, f] : ctx.forecasts) {
        if (only_horizon > 0 && horizon != only_horizon) continue;
        for (const Side side : {Side::long_, Side::short_}) {
            const auto d = evaluate_economics_at(side, horizon, ctx, now_ms, econ);
            const bool act = d.actionable();
            // A SHADOW model is observed on the same terms: what it would have
            // done, recorded, never executable.
            const bool observe = d.state == EconState::model_shadow && d.margin_bps.has_value() &&
                                 d.expected_net_bps > *d.margin_bps;
            if (!act && !observe) continue;
            if (admit && !admit(horizon, side)) continue;

            const bool is_long = side == Side::long_;
            StrategyCandidate c;
            // The state's own time, not only the context revision: between
            // decision bars the same context frames every minute's estimate.
            c.natural_key = ids::join({kStateStrategyId, kStateStrategyVersion, "h" + ids::i64(horizon),
                                       ids::i64(ctx.canonical_pair_id), ids::i64(ctx.context_revision),
                                       ids::i64(ctx.event_time_ms), to_string(side)});
            c.candidate_id = ids::make("cand", c.natural_key);
            c.canonical_pair_id = ctx.canonical_pair_id;
            c.strategy_id = kStateStrategyId;
            c.strategy_version = kStateStrategyVersion;
            c.parameter_set_id = "h" + std::to_string(horizon);
            c.strategy_family = Family::unspecified;   // a state, not a setup family
            c.side = side;
            c.setup_time_ms = ctx.event_time_ms;
            c.created_at_ms = now_ms;
            c.expires_at_ms = now_ms + cfg.candidate_ttl_ms;
            c.context_id = ctx.context_id;
            c.context_revision = ctx.context_revision;
            c.expected_horizon_ms = static_cast<int64_t>(horizon) * 60'000;
            c.holding_class = holding_for(horizon);
            // The model's own geometry, in bps of the entry: adverse <= 0 is
            // against the position, favourable >= 0 with it.
            const double fav = is_long ? f.favorable_long_bps : f.favorable_short_bps;
            const double adv = is_long ? f.adverse_long_bps : f.adverse_short_bps;
            const double dir = is_long ? 1.0 : -1.0;
            c.entry_hint = px;
            c.invalidation_hint = px * (1.0 + dir * std::min(adv, 0.0) / 10'000.0);
            c.target_hint = px * (1.0 + dir * std::max(fav, 0.0) / 10'000.0);
            c.bitget_regime = ctx.bitget_regime.regime;
            c.global_regime = ctx.global_regime.regime;
            c.econ = d;
            c.ml_context = f;
            c.quality = ctx.data_quality;
            c.reason_codes.push_back("ML_STATE");
            c.reason_codes.push_back(to_string(d.state));
            // The setups that described this state, same bar, same side.
            for (const auto& s : setups) {
                if (s.side == side && s.strategy_id != kStateStrategyId) {
                    c.reason_codes.push_back("setup:" + s.strategy_id);
                }
            }
            c.lifecycle_status = act ? Lifecycle::active : Lifecycle::shadow;
            c.selection_mode = act ? SelectionMode::exploit : SelectionMode::explore_shadow;
            // Not a setup: no setup conviction, and no static score to rank by.
            c.raw_setup_score = 0.0;
            c.static_score = 0.0;
            c.adaptive_score = 0.0;
            out.push_back(std::move(c));
        }
    }
    return out;
}

} // namespace altstrategy::engine
