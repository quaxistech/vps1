#include "altstrategy/engine_io/wire_codec.hpp"

#include "decision.pb.h"
#include "altml_forecast.pb.h"
#include "alttech_analytics.pb.h"

#include <cmath>

namespace altstrategy::engine_io {

namespace pbv1 = altstrategy::v1;

namespace {

// ---- domain enum -> altstrategy.v1 proto enum ----
pbv1::Side pb(Side s) {
    switch (s) { case Side::long_: return pbv1::SIDE_LONG; case Side::short_: return pbv1::SIDE_SHORT;
                 default: return pbv1::SIDE_UNSPECIFIED; }
}
pbv1::StrategyFamily pb(Family f) {
    switch (f) { case Family::trend: return pbv1::FAMILY_TREND; case Family::breakout: return pbv1::FAMILY_BREAKOUT;
                 case Family::mean_reversion: return pbv1::FAMILY_MEAN_REVERSION;
                 case Family::cross_venue: return pbv1::FAMILY_CROSS_VENUE; case Family::vwap: return pbv1::FAMILY_VWAP;
                 case Family::crowding: return pbv1::FAMILY_CROWDING; default: return pbv1::FAMILY_UNSPECIFIED; }
}
pbv1::HoldingClass pb(HoldingClass h) {
    switch (h) { case HoldingClass::scalp: return pbv1::HOLDING_SCALP;
                 case HoldingClass::intraday: return pbv1::HOLDING_INTRADAY;
                 case HoldingClass::extended_intraday: return pbv1::HOLDING_EXTENDED_INTRADAY;
                 default: return pbv1::HOLDING_CLASS_UNSPECIFIED; }
}
pbv1::ProposalGrade pb(Grade g) {
    switch (g) { case Grade::strong: return pbv1::GRADE_STRONG; case Grade::normal: return pbv1::GRADE_NORMAL;
                 case Grade::weak: return pbv1::GRADE_WEAK; case Grade::experimental: return pbv1::GRADE_EXPERIMENTAL;
                 case Grade::shadow: return pbv1::GRADE_SHADOW; case Grade::invalid: return pbv1::GRADE_INVALID;
                 default: return pbv1::PROPOSAL_GRADE_UNSPECIFIED; }
}
pbv1::EntryType pb(EntryType e) {
    switch (e) { case EntryType::market: return pbv1::ENTRY_MARKET; case EntryType::limit: return pbv1::ENTRY_LIMIT;
                 case EntryType::stop_market: return pbv1::ENTRY_STOP_MARKET;
                 case EntryType::stop_limit: return pbv1::ENTRY_STOP_LIMIT; default: return pbv1::ENTRY_TYPE_UNSPECIFIED; }
}
pbv1::SelectionMode pb(SelectionMode m) {
    return m == SelectionMode::explore_shadow ? pbv1::SELECTION_EXPLORE_SHADOW
         : m == SelectionMode::exploit ? pbv1::SELECTION_EXPLOIT : pbv1::SELECTION_MODE_UNSPECIFIED;
}
pbv1::PlanProfile pb(PlanProfile p) {
    switch (p) { case PlanProfile::aggressive: return pbv1::PLAN_AGGRESSIVE; case PlanProfile::balanced: return pbv1::PLAN_BALANCED;
                 case PlanProfile::conservative: return pbv1::PLAN_CONSERVATIVE; default: return pbv1::PLAN_PROFILE_UNSPECIFIED; }
}
pbv1::LifecycleStatus pb(Lifecycle l) {
    switch (l) { case Lifecycle::research: return pbv1::LIFECYCLE_RESEARCH; case Lifecycle::backtested: return pbv1::LIFECYCLE_BACKTESTED;
                 case Lifecycle::shadow: return pbv1::LIFECYCLE_SHADOW; case Lifecycle::candidate: return pbv1::LIFECYCLE_CANDIDATE;
                 case Lifecycle::limited: return pbv1::LIFECYCLE_LIMITED; case Lifecycle::active: return pbv1::LIFECYCLE_ACTIVE;
                 case Lifecycle::degraded: return pbv1::LIFECYCLE_DEGRADED; case Lifecycle::disabled: return pbv1::LIFECYCLE_DISABLED;
                 case Lifecycle::retired: return pbv1::LIFECYCLE_RETIRED; default: return pbv1::LIFECYCLE_STATUS_UNSPECIFIED; }
}
pbv1::ContextQuality pb(ContextQuality q) {
    switch (q) { case ContextQuality::strict_4_of_4: return pbv1::CONTEXT_STRICT_4_OF_4;
                 case ContextQuality::degraded_3_of_4: return pbv1::CONTEXT_DEGRADED_3_OF_4;
                 case ContextQuality::degraded_2_of_4: return pbv1::CONTEXT_DEGRADED_2_OF_4;
                 case ContextQuality::invalid: return pbv1::CONTEXT_INVALID; default: return pbv1::CONTEXT_QUALITY_UNSPECIFIED; }
}
pbv1::MarketOutcomeType pb(MarketOutcomeType o) {
    switch (o) { case MarketOutcomeType::entry_not_reached: return pbv1::OUTCOME_ENTRY_NOT_REACHED;
                 case MarketOutcomeType::entry_expired: return pbv1::OUTCOME_ENTRY_EXPIRED;
                 case MarketOutcomeType::target_first: return pbv1::OUTCOME_TARGET_FIRST;
                 case MarketOutcomeType::stop_first: return pbv1::OUTCOME_STOP_FIRST;
                 case MarketOutcomeType::time_stop: return pbv1::OUTCOME_TIME_STOP;
                 case MarketOutcomeType::cancelled: return pbv1::OUTCOME_CANCELLED;
                 case MarketOutcomeType::ambiguous: return pbv1::OUTCOME_AMBIGUOUS;
                 default: return pbv1::OUTCOME_DATA_INCOMPLETE; }
}
pbv1::OutcomePathSource pb(OutcomePath p) {
    switch (p) { case OutcomePath::raw_trades: return pbv1::PATH_RAW_TRADES;
                 case OutcomePath::s1_trade_agg: return pbv1::PATH_1S_TRADE_AGG;
                 case OutcomePath::s1_price_book_agg: return pbv1::PATH_1S_PRICE_BOOK_AGG;
                 default: return pbv1::PATH_1M_CANDLES; }
}
pbv1::DriftStatus pb(DriftStatus d) {
    switch (d) { case DriftStatus::stable: return pbv1::DRIFT_STABLE; case DriftStatus::watch: return pbv1::DRIFT_WATCH;
                 case DriftStatus::degraded: return pbv1::DRIFT_DEGRADED; case DriftStatus::structural_break: return pbv1::DRIFT_STRUCTURAL_BREAK;
                 default: return pbv1::DRIFT_STATUS_UNSPECIFIED; }
}

pbv1::Envelope base_env(const EnvelopeMeta& m) {
    pbv1::Envelope e;
    e.set_schema_version(1);
    e.set_message_id(m.message_id);
    e.set_event_type(m.event_type);
    e.set_canonical_pair_id(m.canonical_pair_id);
    e.set_symbol(m.symbol);
    e.set_trace_id(m.trace_id);
    e.set_correlation_id(m.correlation_id);
    e.set_source_service("altstrategy");
    return e;
}
std::string ser(const pbv1::Envelope& e) { return e.SerializeAsString(); }

void fill_forecast(pbv1::MarketForecastView* mf, const MarketForecastView& v) {
    mf->set_present(v.present);
    mf->set_abstain(v.abstain);
    mf->set_p_up(v.p_up);
    mf->set_p_down(v.p_down);
    mf->set_feature_revision(v.bar_revision);
    mf->set_prediction_id(v.prediction_id);
    mf->set_expected_return(v.expected_return);
    mf->set_probability_target_before_stop(v.probability_target_before_stop);
    mf->set_expected_mfe(v.expected_mfe);
    mf->set_expected_mae(v.expected_mae);
    mf->set_expected_holding_time_min(v.expected_holding_time_min);
    mf->set_conviction(v.conviction);
    mf->set_quality_status(v.quality_status);
    mf->set_model_id(v.model_id);
    mf->set_model_version(v.model_version);
    mf->set_feature_set_version(v.feature_set_version);
    mf->set_prediction_schema_version(v.prediction_schema_version);
    mf->set_bar_open_time_ms(v.bar_open_time_ms);
    mf->set_computed_at_ms(v.computed_at_ms);
    mf->set_econ(v.econ);
    if (v.econ) {
        mf->set_horizon_minutes(v.horizon_minutes);
        mf->set_p_long(v.p_long);
        mf->set_p_neutral(v.p_neutral);
        mf->set_p_short(v.p_short);
        mf->set_expected_net_long_bps(v.expected_net_long_bps);
        mf->set_expected_net_short_bps(v.expected_net_short_bps);
        mf->set_round_trip_cost_bps(v.round_trip_cost_bps);
    }
}

} // namespace

std::string encode_candidate(const StrategyCandidate& c, const EnvelopeMeta& m) {
    auto env = base_env(m);
    auto* x = env.mutable_strategy_candidate();
    x->set_candidate_id(c.candidate_id); x->set_canonical_pair_id(c.canonical_pair_id);
    x->set_strategy_id(c.strategy_id); x->set_strategy_version(c.strategy_version);
    x->set_parameter_set_id(c.parameter_set_id); x->set_strategy_family(pb(c.strategy_family));
    x->set_side(pb(c.side)); x->set_setup_time_ms(c.setup_time_ms); x->set_created_at_ms(c.created_at_ms);
    x->set_expires_at_ms(c.expires_at_ms); x->set_context_id(c.context_id);
    x->set_context_revision(c.context_revision); x->set_raw_setup_score(c.raw_setup_score);
    x->set_static_score(c.static_score); x->set_adaptive_score(c.adaptive_score);
    x->set_strategy_prior(c.strategy_prior); x->set_strategy_posterior(c.strategy_posterior);
    x->set_posterior_uncertainty(c.posterior_uncertainty); x->set_effective_sample_size(c.effective_sample_size);
    x->set_expected_horizon_ms(c.expected_horizon_ms); x->set_holding_class(pb(c.holding_class));
    x->set_entry_hint(c.entry_hint); x->set_invalidation_hint(c.invalidation_hint);
    x->set_target_hint(c.target_hint); x->set_bitget_regime(c.bitget_regime);
    x->set_global_regime(c.global_regime); x->set_cross_venue_confirmation(c.cross_venue_confirmation);
    x->set_quality(pb(c.quality)); x->set_selection_mode(pb(c.selection_mode));
    x->set_lifecycle_status(pb(c.lifecycle_status));
    for (const auto& w : c.warnings) x->add_warnings(w);
    for (const auto& r : c.reason_codes) x->add_reason_codes(r);
    fill_forecast(x->mutable_ml_context(), c.ml_context);
    x->set_econ_state(to_string(c.econ.state));
    x->set_econ_horizon_minutes(c.econ.horizon_minutes);
    if (c.econ.evaluated()) x->set_econ_expected_net_bps(c.econ.expected_net_bps);
    return ser(env);
}

std::string encode_ensemble(const EnsembleView& e, int64_t pair, const EnvelopeMeta& m) {
    auto env = base_env(m);
    auto* x = env.mutable_ranked_proposal();   // ensemble is carried inside ranking; standalone view here
    x->set_canonical_pair_id(pair);
    auto* ev = x->mutable_ensemble();
    ev->set_ensemble_direction(pb(e.ensemble_direction));
    ev->set_ensemble_agreement(e.ensemble_agreement);
    ev->set_family_independence_score(e.family_independence_score);
    ev->set_plan_dispersion(e.plan_dispersion);
    for (const auto& s : e.supporting_strategies) ev->add_supporting_strategies(s);
    for (const auto& s : e.conflicting_strategies) ev->add_conflicting_strategies(s);
    return ser(env);
}

std::string encode_ranking(const RankedProposal& r, const EnvelopeMeta& m) {
    auto env = base_env(m);
    auto* x = env.mutable_ranked_proposal();
    x->set_ranking_id(r.ranking_id); x->set_canonical_pair_id(r.canonical_pair_id);
    x->set_context_id(r.context_id); x->set_ranking_policy_id(r.ranking_policy_id);
    x->set_ranking_policy_version(r.ranking_policy_version); x->set_created_at_ms(r.created_at_ms);
    x->set_ranking_uncertainty(r.ranking_uncertainty); x->set_ranking_regret_estimate(r.ranking_regret_estimate);
    x->set_recommended_candidate_id(r.recommended_candidate_id); x->set_is_challenger(r.is_challenger);
    for (const auto& rc : r.ranked_candidates) {
        auto* c = x->add_ranked_candidates();
        c->set_candidate_id(rc.candidate_id); c->set_strategy_id(rc.strategy_id);
        c->set_strategy_family(pb(rc.strategy_family)); c->set_side(pb(rc.side));
        c->set_final_score(rc.final_score); c->set_adaptive_weight(rc.adaptive_weight);
        c->set_posterior_edge(rc.posterior_edge); c->set_rank(rc.rank);
        c->set_selection_mode(pb(rc.selection_mode));
        for (const auto& [k, v] : rc.components) { c->add_score_component_keys(k); c->add_score_component_values(v); }
    }
    auto* ev = x->mutable_ensemble();
    ev->set_ensemble_direction(pb(r.ensemble.ensemble_direction));
    ev->set_ensemble_agreement(r.ensemble.ensemble_agreement);
    ev->set_family_independence_score(r.ensemble.family_independence_score);
    return ser(env);
}

std::string encode_plan_variant(const TradePlanVariant& v, const EnvelopeMeta& m) {
    auto env = base_env(m);
    auto* x = env.mutable_trade_plan_variant();
    x->set_variant_id(v.variant_id); x->set_candidate_id(v.candidate_id); x->set_context_id(v.context_id);
    x->set_canonical_pair_id(v.canonical_pair_id); x->set_profile(pb(v.profile)); x->set_side(pb(v.side));
    x->set_entry_type(pb(v.entry_type)); x->set_entry_price(v.entry_price); x->set_entry_lower(v.entry_lower);
    x->set_entry_upper(v.entry_upper); x->set_trigger_price(v.trigger_price);
    x->set_stop_loss_price(v.stop_loss_price); x->set_take_profit_price(v.take_profit_price);
    x->set_optional_secondary_target(v.optional_secondary_target);
    x->set_max_holding_time_ms(v.max_holding_time_ms); x->set_time_stop_at_ms(v.time_stop_at_ms);
    x->set_expected_fill_time_p50_min(v.expected_fill_time_p50_min);
    x->set_expected_fill_time_p90_min(v.expected_fill_time_p90_min);
    x->set_entry_fill_timeout_ms(v.entry_fill_timeout_ms); x->set_stop_distance_bps(v.stop_distance_bps);
    x->set_stop_distance_atr(v.stop_distance_atr); x->set_target_distance_bps(v.target_distance_bps);
    x->set_expected_rr(v.expected_rr); x->set_stop_basis(v.stop_basis); x->set_target_basis(v.target_basis);
    x->set_invalidation_reason(v.invalidation_reason); x->set_post_only_allowed(v.post_only_allowed);
    x->set_max_slippage_bps(v.max_slippage_bps);
    return ser(env);
}

std::string encode_proposal(const TradeSignalProposal& s, const EnvelopeMeta& m) {
    auto env = base_env(m);
    auto* x = env.mutable_trade_signal_proposal();
    x->set_signal_proposal_id(s.signal_proposal_id); x->set_candidate_id(s.candidate_id);
    x->set_ranking_id(s.ranking_id); x->set_canonical_pair_id(s.canonical_pair_id);
    x->set_bitget_instrument_id(s.bitget_instrument_id); x->set_strategy_id(s.strategy_id);
    x->set_strategy_version(s.strategy_version); x->set_parameter_set_id(s.parameter_set_id);
    x->set_side(pb(s.side)); x->set_proposal_grade(pb(s.proposal_grade)); x->set_created_at_ms(s.created_at_ms);
    x->set_valid_until_ms(s.valid_until_ms); x->set_entry_type(pb(s.entry_type)); x->set_entry_price(s.entry_price);
    x->set_entry_lower(s.entry_lower); x->set_entry_upper(s.entry_upper); x->set_trigger_price(s.trigger_price);
    x->set_max_slippage_bps(s.max_slippage_bps); x->set_post_only_allowed(s.post_only_allowed);
    x->set_expected_fill_time_p50_min(s.expected_fill_time_p50_min);
    x->set_expected_fill_time_p90_min(s.expected_fill_time_p90_min);
    x->set_entry_fill_timeout_ms(s.entry_fill_timeout_ms); x->set_stop_loss_price(s.stop_loss_price);
    x->set_stop_basis(s.stop_basis); x->set_stop_distance_bps(s.stop_distance_bps);
    x->set_stop_distance_atr(s.stop_distance_atr); x->set_take_profit_price(s.take_profit_price);
    x->set_optional_secondary_target(s.optional_secondary_target); x->set_target_basis(s.target_basis);
    x->set_target_distance_bps(s.target_distance_bps);
    x->set_expected_holding_time_p50_ms(s.expected_holding_time_p50_ms);
    x->set_expected_holding_time_p90_ms(s.expected_holding_time_p90_ms);
    x->set_max_holding_time_ms(s.max_holding_time_ms); x->set_time_stop_at_ms(s.time_stop_at_ms);
    // Forward-looking economics are written only when a model produced them, so
    // a consumer reading 0 from an unset field is reading "absent", which is
    // what edge_estimated says explicitly.
    x->set_edge_estimated(s.edge_estimated);
    if (s.edge_estimated) {
        if (s.p_target_estimated) x->set_p_target_before_stop(s.p_target_before_stop);
        x->set_expected_net_return(s.expected_net_return);
    }
    x->set_expected_rr(s.expected_rr); x->set_ml_conviction(s.ml_conviction);
    x->set_ml_abstain(s.ml_abstain); x->set_strategy_confidence(s.strategy_confidence);
    x->set_ensemble_agreement(s.ensemble_agreement); x->set_adaptive_score(s.adaptive_score);
    x->set_quality(pb(s.quality)); x->set_recommended_for_execution(s.recommended_for_execution);
    x->set_context_revision(s.context_revision); x->set_feature_manifest_hash(s.feature_manifest_hash);
    x->set_ranking_policy_id(s.ranking_policy_id); x->set_plan_version(s.plan_version);
    x->set_holding_class(pb(s.holding_class)); x->set_superseded_by_id(s.superseded_by_id);
    for (const auto& w : s.warnings) {
        auto* pw = x->add_warnings(); pw->set_code(w.code); pw->set_detail(w.detail); pw->set_severity(w.severity);
    }
    for (const auto& f : s.risk_flags) x->add_risk_flags(f);
    for (const auto& r : s.reason_codes) x->add_reason_codes(r);
    for (const auto& a : s.alternative_plan_ids) x->add_alternative_plan_ids(a);
    for (const auto& mv : s.model_versions) x->add_model_versions(mv);
    for (const auto& h : s.market_predictions) {
        auto* mp = x->add_market_predictions();
        mp->set_horizon_minutes(h.horizon_minutes);
        mp->set_state(to_string(h.state));
        mp->set_has_values(h.has_values());
        mp->set_model_id(h.model_id);
        mp->set_model_version(h.model_version);
        mp->set_model_role(h.role);
        mp->set_objective(h.objective);
        mp->set_calibration_version(h.calibrator);
        mp->set_probability_calibrated(h.probability_calibrated);
        mp->set_bar_open_time_ms(h.bar_open_time_ms);
        mp->set_computed_at_ms(h.computed_at_ms);
        mp->set_aligned_missing(h.aligned_missing);
        if (h.has_values()) {
            mp->set_p_up(h.p_up); mp->set_p_flat(h.p_flat); mp->set_p_down(h.p_down);
            mp->set_expected_return_bps(h.expected_return_bps);
            mp->set_expected_up_excursion_bps(h.expected_up_excursion_bps);
            mp->set_expected_down_excursion_bps(h.expected_down_excursion_bps);
            mp->set_cost_model_version(h.cost_model_version);
            mp->set_cost_bps(h.cost_bps);
            mp->set_expected_net_long_bps(h.expected_net_long_bps);
            mp->set_expected_net_short_bps(h.expected_net_short_bps);
        }
    }
    // std::map iterates by name: the descriptors are sorted on the wire.
    for (const auto& [name, v] : s.setup_descriptors) {
        auto* d = x->add_setup_descriptors(); d->set_name(name); d->set_value(v);
    }
    x->set_evaluation_horizon_minutes(s.evaluation_horizon_minutes);
    x->set_signal_semantics(s.signal_semantics);
    return ser(env);
}

std::string encode_market_outcome(const MarketOutcome& o, const EnvelopeMeta& m) {
    auto env = base_env(m);
    auto* x = env.mutable_market_outcome();
    x->set_outcome_id(o.outcome_id); x->set_signal_proposal_id(o.signal_proposal_id);
    x->set_candidate_id(o.candidate_id); x->set_canonical_pair_id(o.canonical_pair_id);
    x->set_outcome(pb(o.outcome)); x->set_path_source(pb(o.path_source));
    x->set_entry_time_ms(o.entry_time_ms); x->set_resolved_time_ms(o.resolved_time_ms);
    x->set_entry_fill_price(o.entry_fill_price); x->set_exit_price(o.exit_price);
    x->set_realized_gross_return(o.realized_gross_return); x->set_realized_net_return(o.realized_net_return);
    x->set_mfe(o.mfe); x->set_mae(o.mae); x->set_holding_time_ms(o.holding_time_ms);
    x->set_ambiguous(o.ambiguous); x->set_is_counterfactual(o.is_counterfactual);
    return ser(env);
}

std::string encode_counterfactual(const CounterfactualOutcome& c, const EnvelopeMeta& m) {
    auto env = base_env(m);
    auto* x = env.mutable_counterfactual_outcome();
    x->set_counterfactual_id(c.counterfactual_id); x->set_ranking_id(c.ranking_id);
    x->set_selected_candidate_id(c.selected_candidate_id); x->set_candidate_id(c.candidate_id);
    x->set_outcome(pb(c.outcome)); x->set_realized_net_return(c.realized_net_return);
    x->set_ranking_regret(c.ranking_regret); x->set_best_hindsight(c.best_hindsight);
    return ser(env);
}





// ------------------------------ parse: AltML --------------------------------

std::optional<ParsedForecast> parse_altml_forecast(std::string_view bytes) {
    // AltML's real message. The previous body parsed a MarketForecast with
    // quantile sub-messages that AltML has never produced -- a contract this
    // service vendored under the same package name as the producer's, and which
    // therefore looked authoritative while describing nothing.
    altml::v1::PredictionEnvelope env;
    if (!env.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) return std::nullopt;
    if (!env.has_prediction()) return std::nullopt;
    const auto& f = env.prediction();

    // The producer states which contract it is speaking and which venue the
    // prediction is ABOUT. Both were decoded and neither was checked: a
    // prediction from a future schema, or one about an information venue, would
    // have been accepted as a Bitget prediction under this engine's field
    // meanings. A version this consumer does not know is not a small difference
    // -- field 19 could mean something else entirely -- and a prediction about
    // Binance is not evidence about the venue this engine trades.
    // The previous schema is still read while a producer is being upgraded:
    // schema 3 carries no calibration verdict (read as uncalibrated) and its
    // excursions as the long side's favourable / adverse moves.
    if (f.prediction_schema_version() != kPredictionSchemaVersion &&
        f.prediction_schema_version() != kPredictionSchemaVersion - 1) {
        return std::nullopt;
    }
    if (f.target_venue() != altml::v1::EXCHANGE_BITGET) return std::nullopt;

    ParsedForecast p;
    p.canonical_pair_id = env.instrument_id();
    p.feature_revision = f.bar_revision();

    auto& v = p.view;
    v.present = true;
    v.abstain = f.abstain();
    v.p_up = f.probability_up();
    v.p_down = f.probability_down();
    v.expected_return = f.expected_return();
    v.probability_target_before_stop = f.probability_target_before_stop();
    v.expected_mfe = f.expected_mfe();
    v.expected_mae = f.expected_mae();
    v.expected_holding_time_min = f.expected_holding_time_min();
    v.conviction = f.confidence();
    v.quality_status = f.quality_status();
    v.model_id = f.model_id();
    v.model_version = f.model_version();
    v.feature_set_version = f.feature_set_version();
    v.prediction_schema_version = f.prediction_schema_version();
    v.bar_open_time_ms = f.bar_open_time_ms();
    v.bar_revision = f.bar_revision();
    v.computed_at_ms = f.computed_at_ms();
    v.prediction_id = f.prediction_id();
    // The econ estimate. Every field it is made of must be PRESENT on the wire:
    // proto3 would read an unset double as 0.0, and a 0.0 net return is a
    // statement ("breaks even") nobody made.
    v.econ = f.horizon_minutes() > 0 && f.has_probability_long() && f.has_probability_neutral() &&
             f.has_probability_short() && f.has_expected_net_return_long_bps() &&
             f.has_expected_net_return_short_bps() && f.has_round_trip_cost_bps();
    if (v.econ) {
        v.horizon_minutes = f.horizon_minutes();
        v.objective = f.objective();
        v.p_long = f.probability_long();
        v.p_neutral = f.probability_neutral();
        v.p_short = f.probability_short();
        v.expected_net_long_bps = f.expected_net_return_long_bps();
        v.expected_net_short_bps = f.expected_net_return_short_bps();
        v.round_trip_cost_bps = f.round_trip_cost_bps();
        v.calibrator = f.calibrator();
        v.recipe_version = f.recipe_version();
        v.aligned_missing = f.aligned_missing();
        if (f.prediction_schema_version() >= 3) {
            v.cost_model_version = f.cost_model_version();
            v.fee_bps = f.fee_bps();
            v.spread_bps = f.spread_bps();
            v.impact_bps = f.impact_bps();
            v.spread_observed = f.spread_observed();
            if (f.has_policy_margin_bps() && std::isfinite(f.policy_margin_bps())) {
                v.policy_margin_bps = f.policy_margin_bps();
            }
            v.policy_version = f.policy_version();
            v.favorable_long_bps = f.favorable_long_bps();
            v.adverse_long_bps = f.adverse_long_bps();
            v.favorable_short_bps = f.favorable_short_bps();
            v.adverse_short_bps = f.adverse_short_bps();
        }
        if (f.prediction_schema_version() >= 4) {
            v.probability_calibrated = f.probability_calibrated();
            v.calibration_version = f.calibration_version();
            v.label_definition = f.label_definition();
            v.has_excursions = f.has_expected_up_excursion_bps() && f.has_expected_down_excursion_bps() &&
                               std::isfinite(f.expected_up_excursion_bps()) &&
                               std::isfinite(f.expected_down_excursion_bps());
            if (v.has_excursions) {
                v.expected_up_excursion_bps = std::max(f.expected_up_excursion_bps(), 0.0);
                v.expected_down_excursion_bps = std::max(f.expected_down_excursion_bps(), 0.0);
            }
            v.magnitude_source = f.magnitude_source();
            v.model_role = f.model_role();
        }
    }
    return p;
}


// ------------------------------ parse: AltTech ------------------------------

namespace at = alttech::v1;

namespace {
ContextQuality from_at(at::CrossVenueQualityStatus q) {
    switch (q) { case at::CROSS_VENUE_QUALITY_STRICT_VALID: return ContextQuality::strict_4_of_4;
                 case at::CROSS_VENUE_QUALITY_DEGRADED_3_OF_4: return ContextQuality::degraded_3_of_4;
                 case at::CROSS_VENUE_QUALITY_DEGRADED_2_OF_4: return ContextQuality::degraded_2_of_4;
                 default: return ContextQuality::invalid; }
}
std::string regime_name(at::GlobalRegime r) {
    switch (r) { case at::GLOBAL_REGIME_TREND_UP: return "TREND_UP"; case at::GLOBAL_REGIME_TREND_DOWN: return "TREND_DOWN";
                 case at::GLOBAL_REGIME_BREAKOUT: return "BREAKOUT"; case at::GLOBAL_REGIME_RANGE: return "RANGE";
                 case at::GLOBAL_REGIME_VOLATILITY_EXPANSION: return "VOLATILITY_EXPANSION";
                 case at::GLOBAL_REGIME_VOLATILITY_COMPRESSION: return "VOLATILITY_COMPRESSION";
                 case at::GLOBAL_REGIME_CHAOTIC: return "CHAOTIC"; case at::GLOBAL_REGIME_LOW_LIQUIDITY: return "LOW_LIQUIDITY";
                 default: return "UNDEFINED"; }
}
std::string market_regime_name(at::MarketRegime r) {
    switch (r) { case at::MARKET_REGIME_TREND_UP: return "TREND_UP"; case at::MARKET_REGIME_TREND_DOWN: return "TREND_DOWN";
                 case at::MARKET_REGIME_BREAKOUT: return "BREAKOUT"; case at::MARKET_REGIME_RANGE: return "RANGE";
                 case at::MARKET_REGIME_VOLATILITY_EXPANSION: return "VOLATILITY_EXPANSION";
                 case at::MARKET_REGIME_VOLATILITY_COMPRESSION: return "VOLATILITY_COMPRESSION";
                 case at::MARKET_REGIME_CHAOTIC: return "CHAOTIC"; case at::MARKET_REGIME_LOW_LIQUIDITY: return "LOW_LIQUIDITY";
                 default: return "UNDEFINED"; }
}
} // namespace

std::optional<ParsedAltTech> parse_alttech(std::string_view bytes) {
    at::Envelope env;
    if (!env.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) return std::nullopt;
    ParsedAltTech p;
    if (env.has_feature_vector()) {
        // Bitget's primary "core" feature vector (field 30). Identity comes
        // from the enclosing Envelope, not from this message; canonical_pair_id
        // is not always populated on this tag, so fall back to instrument_id
        // (== canonical_pair_id for usdt_perp pairs, per alttech's Envelope).
        // Every venue AltTech computes arrives here. Bitget is the target; the
        // other three are information venues whose vectors are evidence, kept in
        // their own summary and never merged into the target's.
        const int32_t venue = static_cast<int32_t>(env.exchange());
        if (venue < 1 || venue > 4) { p.kind = AltTechKind::none; return p; }
        const auto& f = env.feature_vector();
        p.venue = venue;
        p.kind = venue == static_cast<int32_t>(at::EXCHANGE_BITGET)
                     ? AltTechKind::bitget_features
                     : AltTechKind::reference_features;
        p.canonical_pair_id = env.canonical_pair_id() != 0 ? env.canonical_pair_id() : env.instrument_id();
        p.revision = f.bar_revision(); p.bar_open_time_ms = f.bar_open_time_ms();
        p.timeframe_minutes = static_cast<int32_t>(f.timeframe());
        p.values.assign(f.values().begin(), f.values().end());
        p.feature_set_name = f.feature_set_name();
        p.feature_set_version = f.feature_set_version();
        p.schema_hash = f.schema_hash();
        p.validity_bitmap = f.validity_bitmap();
        p.features.bar_open_time_ms = f.bar_open_time_ms(); p.features.revision = f.bar_revision();
        p.features.valid = f.status() == at::FEATURE_STATUS_VALID;
    } else if (env.has_microstructure_vector()) {
        // Primary-venue (Bitget) live book + trade flow. The window start is the
        // reading's timestamp; it is not a bar, so there is no revision.
        if (env.exchange() != at::EXCHANGE_BITGET) { p.kind = AltTechKind::none; return p; }
        const auto& m = env.microstructure_vector();
        p.kind = AltTechKind::bitget_microstructure;
        p.canonical_pair_id = env.canonical_pair_id() != 0 ? env.canonical_pair_id() : env.instrument_id();
        p.window_label = m.window_label();
        p.bar_open_time_ms = m.window_start_ms();
        p.values.assign(m.values().begin(), m.values().end());
        p.feature_set_name = m.feature_set_name();
        p.feature_set_version = m.feature_set_version();
        p.schema_hash = m.schema_hash();
        p.validity_bitmap = m.validity_bitmap();
        p.features.bar_open_time_ms = m.window_start_ms();
        p.features.valid = m.status() == at::MICROSTRUCTURE_STATUS_VALID;
    } else if (env.has_cross_venue_feature_vector()) {
        const auto& f = env.cross_venue_feature_vector();
        p.kind = AltTechKind::cross_venue_features; p.canonical_pair_id = f.canonical_pair_id();
        p.revision = f.analytics_revision(); p.bar_open_time_ms = f.bar_open_time_ms();
        p.quality = from_at(f.quality_status()); p.venue_mask = f.venue_mask(); p.missing_venue_mask = f.missing_venue_mask();
        p.values.assign(f.values().begin(), f.values().end());
        p.feature_set_name = f.feature_set_name();
        p.feature_set_version = f.feature_set_version();
        p.schema_hash = f.schema_hash();
        p.validity_bitmap = f.validity_bitmap();
        p.features.bar_open_time_ms = f.bar_open_time_ms(); p.features.revision = f.analytics_revision();
        p.features.valid = f.status() == at::ANALYTICS_VECTOR_STATUS_VALID;
    } else if (env.has_venue_feature_vector()) {
        const auto& f = env.venue_feature_vector();
        const int32_t venue = static_cast<int32_t>(f.exchange());
        if (venue < 1 || venue > 4) { p.kind = AltTechKind::none; return p; }
        p.venue = venue;
        p.kind = venue == static_cast<int32_t>(at::EXCHANGE_BITGET)
                     ? AltTechKind::bitget_features
                     : AltTechKind::reference_features;
        p.canonical_pair_id = f.canonical_pair_id();
        p.timeframe_minutes = static_cast<int32_t>(f.timeframe());
        p.revision = f.analytics_revision(); p.bar_open_time_ms = f.bar_open_time_ms();
        p.values.assign(f.values().begin(), f.values().end());
        p.feature_set_name = f.feature_set_name();
        p.feature_set_version = f.feature_set_version();
        p.schema_hash = f.schema_hash();
        p.validity_bitmap = f.validity_bitmap();
        p.features.bar_open_time_ms = f.bar_open_time_ms(); p.features.revision = f.analytics_revision();
        p.features.valid = f.status() == at::ANALYTICS_VECTOR_STATUS_VALID;
    } else if (env.has_global_market_regime()) {
        const auto& r = env.global_market_regime();
        p.kind = AltTechKind::global_regime; p.canonical_pair_id = r.canonical_pair_id();
        p.regime.regime = regime_name(r.primary_regime()); p.regime.confidence = r.regime_confidence();
        p.regime.trend_strength = r.trend_consensus(); p.regime.volatility_regime = r.volatility_consensus();
        p.regime.liquidity_quality = r.liquidity_consensus(); p.regime.bar_open_time_ms = r.bar_open_time_ms();
        p.regime_divergence = r.bitget_regime_divergence(); p.quality = from_at(r.quality_status());
    } else if (env.has_venue_market_regime()) {
        const auto& r = env.venue_market_regime();
        if (r.exchange() != at::EXCHANGE_BITGET) { p.kind = AltTechKind::none; return p; }
        p.kind = AltTechKind::bitget_regime; p.canonical_pair_id = r.canonical_pair_id();
        p.regime.regime = regime_name(r.regime()); p.regime.confidence = r.confidence();
        p.regime.trend_strength = r.trend_strength(); p.regime.volatility_regime = r.volatility_regime();
        p.regime.liquidity_quality = r.liquidity_quality(); p.regime.bar_open_time_ms = r.bar_open_time_ms();
    } else if (env.has_combined_regime()) {
        // Only Bitget's: the regime module classifies the execution venue.
        if (env.exchange() != at::EXCHANGE_BITGET) { p.kind = AltTechKind::none; return p; }
        p.kind = AltTechKind::bitget_htf_regime;
        p.canonical_pair_id = env.canonical_pair_id() != 0 ? env.canonical_pair_id() : env.instrument_id();
        for (const auto& t : env.combined_regime().timeframes()) {
            const auto tf = static_cast<int32_t>(t.timeframe());
            if (tf <= 1) continue;   // a 1m entry is not a higher timeframe
            RegimeView v;
            v.regime = market_regime_name(t.regime());
            v.confidence = t.confidence();
            v.trend_strength = t.axes().trend_strength();
            v.volatility_regime = t.axes().volatility_regime();
            v.liquidity_quality = t.axes().liquidity_quality();
            v.bar_open_time_ms = t.bar_open_time_ms();
            p.htf_regimes.emplace_back(tf, v);
        }
        if (p.htf_regimes.empty()) p.kind = AltTechKind::none;
    } else if (env.has_bitget_projected_zone()) {
        const auto& z = env.bitget_projected_zone();
        p.kind = AltTechKind::projected_zone; p.canonical_pair_id = z.canonical_pair_id();
        p.zone.zone_id = z.source_consensus_zone_id(); p.zone.lower = z.projected_lower();
        p.zone.center = z.projected_center(); p.zone.upper = z.projected_upper();
        p.zone.strength = z.projection_quality();
        // A projected zone carries no intrinsic side on the wire: whether it
        // acts as support or resistance follows from where it sits relative to
        // the current price, which only the consumer knows. Tag it honestly and
        // let DecisionContext::nearest_zone() classify it.
        p.zone.kind = "dynamic";
    } else if (env.has_cross_venue_quality()) {
        const auto& q = env.cross_venue_quality();
        p.kind = AltTechKind::cross_venue_quality; p.canonical_pair_id = q.canonical_pair_id();
        p.quality = from_at(q.quality_status()); p.venue_mask = q.venue_mask(); p.missing_venue_mask = q.missing_venue_mask();
    } else if (env.has_universe_snapshot()) {
        p.kind = AltTechKind::universe_snapshot;
    } else if (env.has_universe_change()) {
        p.kind = AltTechKind::universe_change;
        const auto& u = env.universe_change().pair();
        p.canonical_pair_id = u.canonical_pair_id(); p.universe_status = u.status();
    } else {
        p.kind = AltTechKind::none;
    }
    return p;
}

std::optional<ExecutionFeedback> parse_execution_feedback(std::string_view bytes) {
    pbv1::Envelope env;
    if (!env.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) return std::nullopt;
    if (!env.has_execution_feedback()) return std::nullopt;
    const auto& e = env.execution_feedback();
    ExecutionFeedback f;
    f.signal_proposal_id = e.signal_proposal_id(); f.accepted = e.accepted();
    for (const auto& r : e.rejection_reasons()) f.rejection_reasons.push_back(r);
    f.order_submitted = e.order_submitted(); f.order_type = e.order_type();
    f.submitted_at_ms = e.submitted_at_ms(); f.fill_price = e.fill_price();
    f.fill_time_ms = e.fill_time_ms(); f.fill_ratio = e.fill_ratio(); f.slippage_bps = e.slippage_bps();
    f.fees = e.fees(); f.funding = e.funding(); f.position_opened = e.position_opened();
    f.position_closed = e.position_closed(); f.stop_result = e.stop_result();
    f.target_result = e.target_result(); f.time_stop_result = e.time_stop_result();
    f.realized_pnl = e.realized_pnl(); f.mfe = e.mfe(); f.mae = e.mae();
    for (const auto& a : e.execution_anomalies()) f.execution_anomalies.push_back(a);
    f.received_at_ms = e.received_at_ms();
    return f;
}

} // namespace altstrategy::engine_io
