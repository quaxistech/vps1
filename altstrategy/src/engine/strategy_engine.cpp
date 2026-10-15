#include "altstrategy/engine/strategy_engine.hpp"

#include "altstrategy/engine/ids.hpp"
#include "altstrategy/engine/signal_predictions.hpp"
#include "altstrategy/engine/strat_util.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {

namespace {

// Quality ordering: strict(3) > degraded_3(2) > degraded_2(1) > invalid(0).
int quality_rank(ContextQuality q) {
    switch (q) {
        case ContextQuality::strict_4_of_4: return 3;
        case ContextQuality::degraded_3_of_4: return 2;
        case ContextQuality::degraded_2_of_4: return 1;
        default: return 0;
    }
}

bool feature_present(const DecisionContext& ctx, const std::string& name) {
    for (const auto* s : {&ctx.bitget_features, &ctx.cross_venue_features, &ctx.microstructure}) {
        const auto it = s->values.find(name);
        if (it != s->values.end() && std::isfinite(it->second)) return true;
    }
    // Information venues are NOT searched.
    //
    // A prerequisite has to be checked where the strategy will actually read.
    // Every strategy in the catalogue reads ctx.bitget_features,
    // ctx.cross_venue_features or ctx.microstructure; not one reads
    // ctx.reference_features (the only other use of that map in the whole engine
    // was this loop). Answering "is atr_14 present?" with Binance's atr_14 let a
    // strategy past its own prerequisite and then hand it the DEFAULT from an
    // empty Bitget field -- a missing input arriving as a number.
    //
    // The consequence is visible in ablation: keeping only Binance, only OKX or
    // only Bybit produced byte-identical results (4770 trades, 25.5% win, -8.39
    // bps gross in each arm), because no decision depends on which information
    // venue is present. They reach decisions only through the cross-venue
    // consensus, which IS searched above.
    return false;
}

// The class follows the horizon the strategy actually asked for, and the
// declared class bounds it: a strategy that calls itself a scalp cannot be
// promoted to an eight-hour hold because its horizon drifted. `h` was ignored
// entirely -- the `(void)h;` sat after every return, so the declaration a
// strategy makes about its own semantics had no effect on anything.
HoldingClass clamp_holding(HoldingClass declared, int64_t horizon_ms) {
    const HoldingClass from_horizon =
        horizon_ms <= 30LL * 60 * 1000       ? HoldingClass::scalp
        : horizon_ms <= 8LL * 3600 * 1000    ? HoldingClass::intraday
                                             : HoldingClass::extended_intraday;
    const auto rank = [](HoldingClass c) {
        switch (c) {
            case HoldingClass::scalp: return 0;
            case HoldingClass::intraday: return 1;
            case HoldingClass::extended_intraday: return 2;
            default: return 1;
        }
    };
    // Never longer than what the strategy declared for itself.
    return rank(from_horizon) <= rank(declared) ? from_horizon : declared;
}

} // namespace

StrategyEngine::StrategyEngine(StrategyEngineConfig cfg) : cfg_(cfg) {
    strategies_ = StrategyRegistry::instance().create_all();
}

std::vector<StrategyMeta> StrategyEngine::metas() const {
    std::vector<StrategyMeta> out;
    out.reserve(strategies_.size());
    for (const auto& s : strategies_) out.push_back(s->meta());
    return out;
}

bool StrategyEngine::eligible(const StrategyMeta& m, Lifecycle status, const DecisionContext& ctx,
                             std::string* reject) {
    const auto set = [&](const char* r) { if (reject) *reject = r; return false; };
    if (status == Lifecycle::disabled || status == Lifecycle::retired) return set("lifecycle_off");
    // INVALID context or below required quality is ineligible.
    if (ctx.data_quality == ContextQuality::invalid) return set("context_invalid");
    if (quality_rank(ctx.data_quality) < quality_rank(m.required_quality)) return set("quality_below_required");
    // Universe must be eligible (exclusion is a technical INVALID upstream, §5.1).
    if (ctx.global_universe_status.rfind("EXCLUDED", 0) == 0 ||
        ctx.global_universe_status.rfind("MISSING", 0) == 0)
        return set("universe_excluded");
    // Required features must be present and finite.
    for (const auto& fname : m.required_features)
        if (!feature_present(ctx, fname)) return set("missing_feature");
    return true;
}

void StrategyFunnel::merge(const StrategyFunnel& other) {
    for (const auto& [id, o] : other.by_strategy) {
        Counts& c = by_strategy[id];
        c.contexts += o.contexts;
        for (const auto& [reason, n] : o.ineligible) c.ineligible[reason] += n;
        c.evaluated += o.evaluated;
        c.no_setup += o.no_setup;
        c.geometry_rejected += o.geometry_rejected;
        c.horizon_refused += o.horizon_refused;
        c.repeat_within_horizon += o.repeat_within_horizon;
        for (size_t i = 0; i < c.repeat_by_reset_atr.size(); ++i) {
            c.repeat_by_reset_atr[i] += o.repeat_by_reset_atr[i];
            c.readmitted_by_reset_atr[i] += o.readmitted_by_reset_atr[i];
        }
        c.candidates += o.candidates;
        for (const auto& [state, n] : o.econ) c.econ[state] += n;
    }
}

std::vector<StrategyCandidate> StrategyEngine::run(const DecisionContext& ctx, int64_t now_ms,
                                                   const CatalogueState& state,
                                                   StrategyFunnel* funnel, const Admit& admit) const {
    std::vector<StrategyCandidate> out;
    StrategyFunnel::Counts scratch;
    for (const auto& strat : strategies_) {
        const auto& m = strat->meta();
        StrategyFunnel::Counts& fc = funnel ? funnel->by_strategy[m.strategy_id] : scratch;
        ++fc.contexts;
        Lifecycle status = m.default_lifecycle;
        if (auto it = state.lifecycle.find(m.strategy_id); it != state.lifecycle.end())
            status = it->second;
        std::string reject;
        if (!eligible(m, status, ctx, &reject)) { ++fc.ineligible[reject]; continue; }

        ParamValues params = m.default_params();
        if (auto it = state.params.find(m.strategy_id); it != state.params.end()) params = it->second;

        ++fc.evaluated;
        auto raw = strat->evaluate(ctx, params);
        if (!raw) { ++fc.no_setup; continue; }
        RawSignal& r = *raw;
        // Technical geometry guard only — never hide a weak-but-valid setup.
        if (su::raw_rr(r) < std::max(cfg_.min_rr, 0.0) && cfg_.min_rr > 0) { ++fc.geometry_rejected; continue; }
        if (r.side == Side::unspecified) { ++fc.geometry_rejected; continue; }

        SelectionMode mode = SelectionMode::exploit;
        if (auto it = state.selection.find(m.strategy_id); it != state.selection.end())
            mode = it->second;
        else if (status == Lifecycle::shadow || status == Lifecycle::research)
            mode = SelectionMode::explore_shadow;

        StrategyCandidate c;
        c.natural_key = ids::join({m.strategy_id, m.strategy_version, params.parameter_set_id,
                                   ids::i64(ctx.canonical_pair_id), ids::i64(ctx.context_revision),
                                   to_string(r.side)});
        c.candidate_id = ids::make("cand", c.natural_key);
        c.canonical_pair_id = ctx.canonical_pair_id;
        c.strategy_id = m.strategy_id;
        c.strategy_version = m.strategy_version;
        c.parameter_set_id = params.parameter_set_id;
        c.strategy_family = m.family;
        c.side = r.side;
        c.setup_time_ms = ctx.event_time_ms;
        c.created_at_ms = now_ms;
        c.expires_at_ms = now_ms + cfg_.candidate_ttl_ms;
        c.context_id = ctx.context_id;
        c.context_revision = ctx.context_revision;
        c.raw_setup_score = std::clamp(r.raw_setup_score, 0.0, 1.0);
        // Static score: the setup's own conviction tempered by context quality.
        // DIAGNOSTIC ONLY since the ML-first policy: measured out of sample on
        // 21.9k candidates its rank correlation with the realised net outcome
        // was -0.003 / -0.019 / -0.034 at 15m / 1h / 4h, so it no longer ranks
        // or gates anything. It is kept on the record so that finding can be
        // re-measured. It no longer folds in the ML output either -- the model
        // is consulted once, by the economic policy, not twice.
        const double q_factor = 0.7 + 0.1 * quality_rank(ctx.data_quality);
        c.static_score = std::clamp(c.raw_setup_score * q_factor, 0.0, 1.0);
        c.adaptive_score = c.static_score;  // allocator refines this
        // 48h is a refusal boundary, not a clamp. A strategy asking for longer
        // is stating a thesis this engine does not run; truncating it would
        // execute a different idea under the same name.
        if (r.expected_horizon_ms > kMaxHoldingMs) {
            // No candidate at all. This layer is pure logic and does not log;
            // the refusal is observable as the absence of the candidate, and
            // `no candidate produced` is the correct outcome for a thesis the
            // engine does not run.
            ++fc.horizon_refused;
            continue;
        }
        if (admit && !admit(m.strategy_id, r.side, r.expected_horizon_ms)) {
            ++fc.repeat_within_horizon;
            continue;
        }
        c.expected_horizon_ms = r.expected_horizon_ms;
        c.holding_class = clamp_holding(r.holding_class, r.expected_horizon_ms);
        c.entry_hint = r.entry_hint;
        c.invalidation_hint = r.invalidation_hint;
        c.target_hint = r.target_hint;
        c.bitget_regime = ctx.bitget_regime.regime;
        c.global_regime = ctx.global_regime.regime;
        c.cross_venue_confirmation = r.cross_venue_confirmation;
        // The market view on every horizon, and the setup's measured
        // description: what a VPS1 signal is made of.
        c.predictions = market_predictions(ctx, now_ms, cfg_.econ.max_forecast_age_ms);
        c.descriptors = r.descriptors;
        // The economic DIAGNOSTIC for this side over this setup's horizon:
        // recorded on every candidate, gating none.
        c.econ = evaluate_economics(r.side, r.expected_horizon_ms, ctx, now_ms, cfg_.econ);
        if (const auto f = ctx.forecasts.find(c.econ.horizon_minutes); f != ctx.forecasts.end()) {
            c.ml_context = f->second;
        }
        ++fc.econ[to_string(c.econ.state)];
        c.quality = ctx.data_quality;
        c.warnings = r.warnings;
        c.reason_codes = r.reason_codes;
        c.selection_mode = mode;
        c.lifecycle_status = status;
        out.push_back(std::move(c));
        ++fc.candidates;
    }
    return out;
}

} // namespace altstrategy::engine

