#include "altstrategy/engine_io/decision_store.hpp"

#include "altstrategy/engine_io/context_json.hpp"

#include "altstrategy/engine/adapt.hpp"

#include "altstrategy/logging/logger.hpp"
#include "altstrategy/persistence/pg.hpp"

#include <pqxx/pqxx>

#include <cctype>
#include <cmath>
#include <cstdio>

#include <sstream>

namespace altstrategy::engine_io {

namespace {
// An economics figure nobody estimated is written as NULL. Binding 0.0 would
// store "the expected return is zero", which is a different claim.
std::optional<double> opt_double(bool present, double value) {
    return present ? std::optional<double>{value} : std::nullopt;
}
}  // namespace


namespace {
// A read that FAILS must never look like a read that found NOTHING.
//
// Every loader in this file returns a container and drives a decision from it:
// posteriors, drift inputs, lifecycle overrides, calibration targets. Twelve of
// them discarded the transaction's Result, so a database error — a timeout, a
// conversion failure, a dropped connection — produced an empty container and
// the caller carried on as though PostgreSQL had told it there was no evidence.
//
// It cost the adaptive layer entirely. `strategy_posteriors` sat empty while
// 24,960 usable outcomes were queryable, so every candidate carried a posterior
// edge of exactly 0, every lifecycle transition condition (`edge < 0` to demote,
// `edge > 0.001` to recover) was false by construction, and three strategies
// were frozen in DEGRADED with `lifecycle_transitions: 0` on every scan. The
// failure said nothing; the system simply behaved as if it had no history.
template <typename Fn>
bool read_txn(persistence::PgPool& pool, const char* query, Fn&& fn) {
    auto res = pool.with_txn(std::forward<Fn>(fn));
    if (!res) {
        log::error("decision_store_read_failed",
                   {{"query", query}, {"message", res.error().message}});
        return false;
    }
    return true;
}
}  // namespace


namespace {

// PostgreSQL text[] literal from a vector of strings (quoted + escaped).
std::string pg_array(const std::vector<std::string>& v) {
    std::string s = "{";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        s += '"';
        for (char ch : v[i]) { if (ch == '"' || ch == '\\') s += '\\'; s += ch; }
        s += '"';
    }
    s += "}";
    return s;
}

std::string json_of(const std::map<std::string, std::string>& m) {
    std::ostringstream os;
    os << '{';
    bool first = true;
    for (const auto& [k, v] : m) { if (!first) os << ','; first = false; os << '"' << k << "\":\"" << v << "\""; }
    os << '}';
    return os.str();
}

} // namespace

namespace {
// A write that fails must say so. Fourteen store functions returned a bare
// `r.has_value()`, so a rejected transaction — a CHECK violation, a constraint
// clash, a dead connection — surfaced as a plain `false` that every caller then
// treated as "nothing to do". That is how a calibrated parameter set was
// computed correctly and silently discarded for hours.
template <typename R>
bool ok(const char* op, const R& r) {
    if (r) return true;
    log::warn("db_write_failed", {{"op", op}, {"message", r.error().message}});
    return false;
}
}  // namespace

bool DecisionStore::save_context_snapshot(const DecisionContext& c) {
    const ObservedScalars sc = observed_scalars(c);   // NULL where not observed
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.decision_context_snapshots("
            "context_id,canonical_pair_id,bitget_instrument_id,context_revision,event_time_ms,"
            "available_at_ms,expires_at_ms,global_universe_status,data_quality,venue_mask,"
            "missing_venue_mask,regime_divergence,bitget_regime,global_regime,feature_manifest_hash,"
            "producer_versions,fees_bps,funding_rate,mark_price,spread_bps,top_depth_usdt,"
            "liquidity_rank,expected_slippage_bps,spread_observed,depth_observed,snapshot) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16::jsonb,$17,$18,$19,"
            "$20,$21,$22,$23,$24,$25,$26::jsonb) "
            "ON CONFLICT (context_id) DO NOTHING",
            c.context_id, c.canonical_pair_id, c.bitget_instrument_id, c.context_revision,
            c.event_time_ms, c.available_at_ms, c.expires_at_ms, c.global_universe_status,
            to_string(c.data_quality), static_cast<int>(c.venue_mask), static_cast<int>(c.missing_venue_mask),
            c.regime_divergence, c.bitget_regime.regime, c.global_regime.regime, c.feature_manifest_hash,
            json_of(c.producer_versions), c.fees_bps, sc.funding_rate, c.mark_price, sc.spread_bps,
            sc.top_depth_usdt, sc.liquidity_rank, c.expected_slippage_bps,
            c.spread_observed, c.depth_observed,
            context_snapshot_json(c));
    });
    return ok("save_context_snapshot", r);
}

bool DecisionStore::save_candidate(const StrategyCandidate& c) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.strategy_candidates("
            "candidate_id,natural_key,canonical_pair_id,strategy_id,strategy_version,parameter_set_id,"
            "strategy_family,side,setup_time_ms,created_at_ms,expires_at_ms,context_id,context_revision,"
            "raw_setup_score,static_score,adaptive_score,strategy_prior,strategy_posterior,"
            "posterior_uncertainty,effective_sample_size,expected_horizon_ms,holding_class,entry_hint,"
            "invalidation_hint,target_hint,bitget_regime,global_regime,cross_venue_confirmation,"
            "selection_mode,lifecycle_status,quality,econ_state,econ_horizon_minutes,"
            "econ_expected_net_bps,econ_p_side,econ_p_opposite,econ_round_trip_cost_bps,econ_model_id,"
            "econ_forecast_bar_ms) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21,$22,$23,"
            "$24,$25,$26,$27,$28,$29,$30,$31,$32,$33,$34,$35,$36,$37,$38,$39) "
            "ON CONFLICT (natural_key) DO NOTHING",
            c.candidate_id, c.natural_key, c.canonical_pair_id, c.strategy_id, c.strategy_version,
            c.parameter_set_id, to_string(c.strategy_family), to_string(c.side), c.setup_time_ms,
            c.created_at_ms, c.expires_at_ms, c.context_id, c.context_revision, c.raw_setup_score,
            c.static_score, c.adaptive_score, c.strategy_prior, c.strategy_posterior,
            c.posterior_uncertainty, c.effective_sample_size, c.expected_horizon_ms,
            to_string(c.holding_class), c.entry_hint, c.invalidation_hint, c.target_hint,
            c.bitget_regime, c.global_regime, c.cross_venue_confirmation, to_string(c.selection_mode),
            to_string(c.lifecycle_status), to_string(c.quality), to_string(c.econ.state),
            c.econ.horizon_minutes,
            // A candidate with no estimate has NULL numbers; a SHADOW estimate is
            // stored (econ_state says it was shadow) -- it is what a live model
            // said, kept for learning, never what the decision acted on.
            c.econ.has_estimate() ? std::optional<double>(c.econ.expected_net_bps) : std::nullopt,
            c.econ.has_estimate() ? std::optional<double>(c.econ.p_side) : std::nullopt,
            c.econ.has_estimate() ? std::optional<double>(c.econ.p_opposite) : std::nullopt,
            c.econ.has_estimate() ? std::optional<double>(c.econ.round_trip_cost_bps) : std::nullopt,
            c.econ.model_id.empty() ? std::optional<std::string>() : std::optional<std::string>(c.econ.model_id),
            c.econ.forecast_bar_open_ms > 0 ? std::optional<int64_t>(c.econ.forecast_bar_open_ms)
                                            : std::nullopt);
        if (!c.descriptors.empty()) {
            txn.exec_params("UPDATE altstrategy.strategy_candidates SET descriptors = $2::jsonb "
                            "WHERE candidate_id = $1 AND descriptors IS NULL",
                            c.candidate_id, json_of(c.descriptors));
        }
        // The market view on every served horizon. A horizon without numbers
        // is still a row: MISSING / ABSTAIN / INCOMPATIBLE is a finding.
        for (const auto& h : c.predictions) {
            const bool v = h.has_values();
            const auto num = [&](double x) { return v && std::isfinite(x) ? std::optional<double>(x) : std::nullopt; };
            const auto txt = [](const std::string& x) {
                return x.empty() ? std::optional<std::string>() : std::optional<std::string>(x);
            };
            txn.exec_params(
                "INSERT INTO altstrategy.candidate_predictions (candidate_id, horizon_minutes, state, "
                "model_id, model_version, model_role, calibration_version, probability_calibrated, "
                "p_up, p_flat, p_down, expected_return_bps, expected_up_excursion_bps, "
                "expected_down_excursion_bps, bar_open_time_ms, computed_at_ms, aligned_missing, "
                "cost_model_version, cost_bps, expected_net_long_bps, expected_net_short_bps) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21) "
                "ON CONFLICT DO NOTHING",
                c.candidate_id, h.horizon_minutes, to_string(h.state), txt(h.model_id),
                h.model_id.empty() ? std::optional<int32_t>() : std::optional<int32_t>(h.model_version),
                txt(h.role), txt(h.calibrator), h.probability_calibrated, num(h.p_up), num(h.p_flat),
                num(h.p_down), num(h.expected_return_bps), num(h.expected_up_excursion_bps),
                num(h.expected_down_excursion_bps),
                h.bar_open_time_ms > 0 ? std::optional<int64_t>(h.bar_open_time_ms) : std::nullopt,
                h.computed_at_ms > 0 ? std::optional<int64_t>(h.computed_at_ms) : std::nullopt,
                h.model_id.empty() ? std::optional<int32_t>() : std::optional<int32_t>(h.aligned_missing),
                v ? txt(h.cost_model_version) : std::nullopt, num(h.cost_bps),
                num(h.expected_net_long_bps), num(h.expected_net_short_bps));
        }
        int ord = 0;
        for (const auto& rc : c.reason_codes)
            txn.exec_params("INSERT INTO altstrategy.strategy_candidate_reasons(candidate_id,ordinal,kind,code)"
                            " VALUES($1,$2,'reason',$3) ON CONFLICT DO NOTHING", c.candidate_id, ord++, rc);
        for (const auto& w : c.warnings)
            txn.exec_params("INSERT INTO altstrategy.strategy_candidate_reasons(candidate_id,ordinal,kind,code)"
                            " VALUES($1,$2,'warning',$3) ON CONFLICT DO NOTHING", c.candidate_id, ord++, w);
    });
    return ok("save_candidate", r);
}

std::vector<DecisionStore::PendingCandidate> DecisionStore::candidates_awaiting_market_outcome(
    int64_t now_ms, int limit) {
    std::vector<PendingCandidate> out;
    // Every horizon (4 h) plus five minutes for the last candle to settle;
    // three days back at most -- older gaps are data problems, not backlog.
    const int64_t upto = now_ms - (240 + 5) * 60'000LL;
    const int64_t from = now_ms - 3 * 86'400'000LL;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        for (const auto& row : txn.exec_params(
                 "SELECT c.candidate_id, c.canonical_pair_id, c.side, c.created_at_ms, "
                 "       c.econ_round_trip_cost_bps "
                 "FROM altstrategy.strategy_candidates c "
                 "WHERE c.created_at_ms <= $1 AND c.created_at_ms >= $2 "
                 "  AND NOT EXISTS (SELECT 1 FROM altstrategy.candidate_market_outcomes o "
                 "                  WHERE o.candidate_id = c.candidate_id) "
                 "ORDER BY c.created_at_ms DESC LIMIT $3",
                 upto, from, limit)) {
            PendingCandidate p;
            p.candidate_id = row[0].as<std::string>();
            p.canonical_pair_id = row[1].as<int64_t>();
            p.side = side_from_string(row[2].as<std::string>());
            p.created_at_ms = row[3].as<int64_t>();
            if (!row[4].is_null()) p.cost_bps = row[4].as<double>();
            out.push_back(std::move(p));
        }
    });
    ok("candidates_awaiting_market_outcome", r);
    return out;
}

bool DecisionStore::save_candidate_market_outcome(const CandidateMarketOutcome& o) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.candidate_market_outcomes (candidate_id, horizon_minutes, "
            "entry_ref_ms, entry_price, exit_price, gross_bps, mfe_bps, mae_bps, cost_bps, net_bps, "
            "t_mfe_min, t_mae_min) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12) ON CONFLICT DO NOTHING",
            o.candidate_id, o.horizon_minutes, o.entry_ref_ms, o.entry_price, o.exit_price,
            o.gross_bps, o.mfe_bps, o.mae_bps, o.cost_bps, o.net_bps, o.t_mfe_min, o.t_mae_min);
    });
    return ok("save_candidate_market_outcome", r);
}

bool DecisionStore::save_ranking(const RankedProposal& r) {
    auto res = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.strategy_rankings(ranking_id,natural_key,canonical_pair_id,context_id,"
            "ranking_policy_id,ranking_policy_version,is_challenger,result,recommended_candidate_id,"
            "ensemble_id,ranking_uncertainty,ranking_regret_estimate,created_at_ms) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13) ON CONFLICT (natural_key) DO NOTHING",
            r.ranking_id, r.natural_key, r.canonical_pair_id, r.context_id, r.ranking_policy_id,
            r.ranking_policy_version, r.is_challenger, to_string(r.result), r.recommended_candidate_id,
            r.ensemble.ensemble_id, r.ranking_uncertainty, r.ranking_regret_estimate, r.created_at_ms);
        for (const auto& rc : r.ranked_candidates)
            txn.exec_params(
                "INSERT INTO altstrategy.ranking_components(ranking_id,candidate_id,rank,final_score,"
                "adaptive_weight,posterior_edge,components) VALUES($1,$2,$3,$4,$5,$6,$7::jsonb) "
                "ON CONFLICT (ranking_id,candidate_id) DO NOTHING",
                r.ranking_id, rc.candidate_id, rc.rank, rc.final_score, rc.adaptive_weight,
                rc.posterior_edge, json_of(rc.components));
    });
    return res.has_value();
}

bool DecisionStore::save_plan_variant(const TradePlanVariant& v) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.trade_plan_variants(variant_id,natural_key,candidate_id,context_id,"
            "canonical_pair_id,profile,side,entry_type,entry_price,entry_lower,entry_upper,trigger_price,"
            "stop_loss_price,take_profit_price,optional_secondary_target,max_holding_time_ms,time_stop_at_ms,"
            "expected_fill_time_p50_min,expected_fill_time_p90_min,entry_fill_timeout_ms,stop_distance_bps,"
            "stop_distance_atr,target_distance_bps,expected_rr,stop_basis,target_basis,invalidation_reason) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21,$22,$23,$24,"
            "$25,$26,$27) ON CONFLICT (natural_key) DO NOTHING",
            v.variant_id, v.natural_key, v.candidate_id, v.context_id, v.canonical_pair_id,
            to_string(v.profile), to_string(v.side), to_string(v.entry_type), v.entry_price, v.entry_lower,
            v.entry_upper, v.trigger_price, v.stop_loss_price, v.take_profit_price,
            v.optional_secondary_target, v.max_holding_time_ms, v.time_stop_at_ms,
            v.expected_fill_time_p50_min, v.expected_fill_time_p90_min, v.entry_fill_timeout_ms,
            v.stop_distance_bps, v.stop_distance_atr, v.target_distance_bps, v.expected_rr, v.stop_basis,
            v.target_basis, v.invalidation_reason);
    });
    return ok("save_plan_variant", r);
}

bool DecisionStore::save_proposal(const TradeSignalProposal& s) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.trade_signal_proposals(signal_proposal_id,natural_key,candidate_id,"
            "ranking_id,canonical_pair_id,bitget_instrument_id,strategy_id,strategy_version,parameter_set_id,"
            "side,proposal_grade,plan_version,holding_class,created_at_ms,valid_until_ms,entry_type,"
            "entry_price,entry_lower,entry_upper,trigger_price,max_slippage_bps,post_only_allowed,"
            "expected_fill_time_p50_min,expected_fill_time_p90_min,entry_fill_timeout_ms,stop_loss_price,"
            "stop_basis,stop_distance_bps,stop_distance_atr,take_profit_price,optional_secondary_target,"
            "target_basis,target_distance_bps,expected_holding_time_p50_ms,expected_holding_time_p90_ms,"
            "max_holding_time_ms,time_stop_at_ms,edge_estimated,p_target_before_stop,"
            "expected_net_return,expected_rr,"
            "ml_conviction,ml_abstain,strategy_confidence,ensemble_agreement,adaptive_score,quality,"
            "risk_flags,reason_codes,recommended_for_execution,alternative_plan_ids,context_revision,"
            "feature_manifest_hash,model_versions,ranking_policy_id,state,"
            "cost_model_version,fee_bps_round_trip,spread_cost_bps,slippage_cost_bps,"
            "round_trip_cost_bps,spread_observed) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21,$22,$23,"
            "$24,$25,$26,$27,$28,$29,$30,$31,$32,$33,$34,$35,$36,$37,$38,$39,$40,$41,$42,$43,"
            "$44,$45,$46,$47,$48::text[],$49::text[],$50,$51::text[],$52,$53,$54::text[],$55,"
            "$56,$57,$58,$59,$60,$61,$62) "
            "ON CONFLICT (natural_key) DO NOTHING",
            s.signal_proposal_id, s.natural_key, s.candidate_id, s.ranking_id, s.canonical_pair_id,
            s.bitget_instrument_id, s.strategy_id, s.strategy_version, s.parameter_set_id, to_string(s.side),
            to_string(s.proposal_grade), s.plan_version, to_string(s.holding_class), s.created_at_ms,
            s.valid_until_ms, to_string(s.entry_type), s.entry_price, s.entry_lower, s.entry_upper,
            s.trigger_price, s.max_slippage_bps, s.post_only_allowed, s.expected_fill_time_p50_min,
            s.expected_fill_time_p90_min, s.entry_fill_timeout_ms, s.stop_loss_price, s.stop_basis,
            s.stop_distance_bps, s.stop_distance_atr, s.take_profit_price, s.optional_secondary_target,
            s.target_basis, s.target_distance_bps, s.expected_holding_time_p50_ms,
            s.expected_holding_time_p90_ms, s.max_holding_time_ms, s.time_stop_at_ms,
            s.edge_estimated, opt_double(s.edge_estimated && s.p_target_estimated, s.p_target_before_stop),
            opt_double(s.edge_estimated, s.expected_net_return), s.expected_rr,
            s.ml_conviction, s.ml_abstain, s.strategy_confidence, s.ensemble_agreement,
            s.adaptive_score, to_string(s.quality), pg_array(s.risk_flags), pg_array(s.reason_codes),
            s.recommended_for_execution, pg_array(s.alternative_plan_ids), s.context_revision,
            s.feature_manifest_hash, pg_array(s.model_versions), s.ranking_policy_id, to_string(s.state),
            s.cost_model_version, s.fee_bps_round_trip, s.spread_cost_bps, s.slippage_cost_bps,
            s.round_trip_cost_bps, s.cost_spread_observed);
        int ord = 0;
        for (const auto& w : s.warnings)
            txn.exec_params("INSERT INTO altstrategy.proposal_warnings(signal_proposal_id,ordinal,code,detail,"
                            "severity) VALUES($1,$2,$3,$4,$5) ON CONFLICT DO NOTHING",
                            s.signal_proposal_id, ord++, w.code, w.detail, w.severity);
        txn.exec_params("INSERT INTO altstrategy.proposal_state_events(signal_proposal_id,ordinal,from_state,"
                        "to_state,reason,at_ms) VALUES($1,0,NULL,$2,'created',$3) ON CONFLICT DO NOTHING",
                        s.signal_proposal_id, to_string(s.state), s.created_at_ms);
    });
    return ok("save_proposal", r);
}

bool DecisionStore::save_market_outcome(const MarketOutcome& o) {
    std::optional<std::string> proposal_id;
    if (!o.signal_proposal_id.empty()) proposal_id = o.signal_proposal_id;
    // Report whether an outcome for THIS row now exists, not merely whether the
    // transaction committed. A conflicting insert that does nothing used to
    // report success, so the caller marked the proposal final while the
    // maturation query — which looks for an outcome linked to this proposal —
    // still saw it as pending. It was then re-selected on every pass and wrote
    // another OUTCOME_FINAL state event, forever.
    bool stored = false;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        const auto res = txn.exec_params(
            "INSERT INTO altstrategy.market_outcomes(outcome_id,signal_proposal_id,candidate_id,"
            "canonical_pair_id,outcome,path_source,entry_time_ms,resolved_time_ms,entry_fill_price,"
            "exit_price,realized_gross_return,realized_net_return,mfe,mae,holding_time_ms,ambiguous,"
            "is_counterfactual,cost_model_version,round_trip_cost_bps) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19) "
            "ON CONFLICT DO NOTHING RETURNING outcome_id",
            o.outcome_id, proposal_id, o.candidate_id, o.canonical_pair_id, to_string(o.outcome),
            to_string(o.path_source), o.entry_time_ms, o.resolved_time_ms, o.entry_fill_price, o.exit_price,
            o.realized_gross_return, o.realized_net_return, o.mfe, o.mae, o.holding_time_ms, o.ambiguous,
            o.is_counterfactual, o.cost_model_version, o.round_trip_cost_bps);
        if (!res.empty()) { stored = true; return; }
        // Already recorded by an earlier pass: idempotent, and still "stored".
        const auto existing = txn.exec_params(
            "SELECT 1 FROM altstrategy.market_outcomes "
            "WHERE is_counterfactual = $2 AND ("
            "  ($2 AND candidate_id = $3) OR (NOT $2 AND signal_proposal_id = $1))",
            proposal_id, o.is_counterfactual, o.candidate_id);
        stored = !existing.empty();
    });
    return r.has_value() && stored;
}

bool DecisionStore::save_counterfactual(const CounterfactualOutcome& c) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.counterfactual_outcomes(counterfactual_id,ranking_id,"
            "selected_candidate_id,candidate_id,plan_version,outcome,realized_net_return,ranking_regret,"
            "best_hindsight) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9) "
            "ON CONFLICT (ranking_id,candidate_id,plan_version) DO NOTHING",
            c.counterfactual_id, c.ranking_id, c.selected_candidate_id, c.candidate_id,
            c.plan_version, to_string(c.outcome), c.realized_net_return, c.ranking_regret,
            c.best_hindsight);
    });
    return ok("save_counterfactual", r);
}

bool DecisionStore::save_attribution(const OutcomeAttribution& a) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.outcome_attribution(attribution_id,signal_proposal_id,"
            "strategy_attribution,ranking_attribution,geometry_attribution,ml_attribution,"
            "execution_attribution,cost_attribution,regime_attribution,dominant_factor) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10) ON CONFLICT (signal_proposal_id) DO NOTHING",
            a.attribution_id, a.signal_proposal_id, a.strategy_attribution, a.ranking_attribution,
            a.geometry_attribution, a.ml_attribution, a.execution_attribution, a.cost_attribution,
            a.regime_attribution, a.dominant_factor);
    });
    return ok("save_attribution", r);
}

bool DecisionStore::save_execution_feedback(const ExecutionFeedback& f) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.execution_feedback(signal_proposal_id,accepted,rejection_reasons,"
            "order_submitted,order_type,submitted_at_ms,fill_price,fill_time_ms,fill_ratio,slippage_bps,"
            "fees,funding,position_opened,position_closed,stop_result,target_result,time_stop_result,"
            "realized_pnl,mfe,mae,execution_anomalies,received_at_ms) "
            "VALUES($1,$2,$3::text[],$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21::text[],$22) "
            "ON CONFLICT (signal_proposal_id) DO NOTHING",
            f.signal_proposal_id, f.accepted, pg_array(f.rejection_reasons), f.order_submitted, f.order_type,
            f.submitted_at_ms, f.fill_price, f.fill_time_ms, f.fill_ratio, f.slippage_bps, f.fees, f.funding,
            f.position_opened, f.position_closed, f.stop_result, f.target_result, f.time_stop_result,
            f.realized_pnl, f.mfe, f.mae, pg_array(f.execution_anomalies), f.received_at_ms);
    });
    return ok("save_execution_feedback", r);
}

bool DecisionStore::save_posterior(const StrategyPosterior& p) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.strategy_posteriors(strategy_id,strategy_version,parameter_set_id,"
            "regime,symbol_group,canonical_pair_id,side,holding_class,strategy_prior,posterior_mean_edge,"
            "posterior_uncertainty,effective_sample_size,shrinkage_factor,time_decay_state,adaptive_weight,"
            "sample_count,updated_at_ms) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17) "
            "ON CONFLICT (strategy_id,strategy_version,parameter_set_id,regime,symbol_group,canonical_pair_id,"
            "side,holding_class) DO UPDATE SET posterior_mean_edge=EXCLUDED.posterior_mean_edge,"
            "posterior_uncertainty=EXCLUDED.posterior_uncertainty,effective_sample_size=EXCLUDED.effective_sample_size,"
            "shrinkage_factor=EXCLUDED.shrinkage_factor,adaptive_weight=EXCLUDED.adaptive_weight,"
            "sample_count=EXCLUDED.sample_count,updated_at_ms=EXCLUDED.updated_at_ms,updated_at=now()",
            p.strategy_id, p.strategy_version, p.parameter_set_id, p.regime, p.symbol_group,
            p.canonical_pair_id, to_string(p.side), to_string(p.holding_class), p.strategy_prior,
            p.posterior_mean_edge, p.posterior_uncertainty, p.effective_sample_size, p.shrinkage_factor,
            p.time_decay_state, p.adaptive_weight, p.sample_count, p.updated_at_ms);
    });
    return ok("save_posterior", r);
}

bool DecisionStore::save_drift(const StrategyDrift& d) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        const std::string id = "drift_" + d.strategy_id + "_" + std::to_string(d.computed_at_ms);
        txn.exec_params(
            "INSERT INTO altstrategy.strategy_drift_snapshots(drift_id,strategy_id,strategy_version,status,"
            "candidate_frequency_drift,score_distribution_drift,outcome_drift,fill_time_drift,"
            "holding_time_drift,mfe_mae_drift,regime_performance_drift,symbol_performance_drift,reasons,"
            "computed_at_ms) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13::text[],$14) "
            "ON CONFLICT (drift_id) DO NOTHING",
            id, d.strategy_id, d.strategy_version, to_string(d.status), d.candidate_frequency_drift,
            d.score_distribution_drift, d.outcome_drift, d.fill_time_drift, d.holding_time_drift,
            d.mfe_mae_drift, d.regime_performance_drift, d.symbol_performance_drift, pg_array(d.reasons),
            d.computed_at_ms);
    });
    return ok("save_drift", r);
}

bool DecisionStore::lifecycle_cooldown_elapsed(const std::string& strategy_id, int64_t now_ms,
                                               int64_t cooldown_ms) {
    bool elapsed = true;
    read_txn(pool_, "lifecycle_cooldown_elapsed", [&](pqxx::work& txn) {
        const auto r = txn.exec_params(
            "SELECT COALESCE(max(created_at_ms),0) FROM altstrategy.lifecycle_events "
            "WHERE strategy_id = $1",
            strategy_id);
        const int64_t last = r.empty() ? 0 : r[0][0].as<int64_t>();
        elapsed = last == 0 || (now_ms - last) >= cooldown_ms;
    });
    return elapsed;
}

bool DecisionStore::save_lifecycle_transition(const LifecycleTransition& t) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        const std::string id = "lc_" + t.strategy_id + "_" + std::to_string(t.created_at_ms);
        txn.exec_params(
            "INSERT INTO altstrategy.lifecycle_events(event_id,strategy_id,strategy_version,from_status,"
            "to_status,reason,sample_count,rollback,policy_version,created_at_ms) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10) ON CONFLICT (event_id) DO NOTHING",
            id, t.strategy_id, t.strategy_version, to_string(t.from_status), to_string(t.to_status),
            t.reason, t.sample_count, t.rollback, t.policy_version, t.created_at_ms);
        txn.exec_params(
            "INSERT INTO altstrategy.strategy_lifecycle(strategy_id,strategy_version,lifecycle_status,updated_at) "
            "VALUES($1,$2,$3,now()) ON CONFLICT (strategy_id) DO UPDATE SET "
            "lifecycle_status=EXCLUDED.lifecycle_status,strategy_version=EXCLUDED.strategy_version,updated_at=now()",
            t.strategy_id, t.strategy_version, to_string(t.to_status));
    });
    return ok("save_lifecycle_transition", r);
}

bool DecisionStore::upsert_training_example(const TradeSignalProposal& s, const MarketOutcome& o,
                                            const DecisionContext& ctx, const std::string& label_spec_hash) {
    // Ambiguous / incomplete outcomes are archived but not dataset-eligible (§24).
    const bool binary_usable = o.outcome != MarketOutcomeType::ambiguous &&
                               o.outcome != MarketOutcomeType::data_incomplete;
    const bool eligible = binary_usable && s.proposal_grade != Grade::invalid;
    const std::string example_id = "ex_" + s.signal_proposal_id;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.signal_training_examples(example_id,signal_proposal_id,candidate_id,"
            "observation_time_ms,available_at_ms,label_matured_at_ms,canonical_pair_id,strategy_id,"
            "strategy_version,parameter_set_id,side,proposal_grade,feature_manifest_hash,label_spec_hash,"
            "quality,context_id,input_features,dataset_eligible) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17::jsonb,$18) "
            "ON CONFLICT (signal_proposal_id,plan_variant_id) DO NOTHING",
            example_id, s.signal_proposal_id, s.candidate_id, ctx.event_time_ms, ctx.available_at_ms,
            o.resolved_time_ms, s.canonical_pair_id, s.strategy_id, s.strategy_version, s.parameter_set_id,
            to_string(s.side), to_string(s.proposal_grade), s.feature_manifest_hash, label_spec_hash,
            to_string(s.quality), ctx.context_id, json_of(ctx.bitget_features.values), eligible);
        txn.exec_params(
            "INSERT INTO altstrategy.signal_training_labels(example_id,categorical_label,"
            "net_return_after_costs,mfe,mae,holding_duration_ms,binary_label_usable,label_spec_hash) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8) ON CONFLICT (example_id) DO NOTHING",
            example_id, to_string(o.outcome), o.realized_net_return, o.mfe, o.mae, o.holding_time_ms,
            binary_usable, label_spec_hash);
    });
    return ok("upsert_training_example", r);
}

std::vector<OutcomeSample> DecisionStore::load_outcome_samples(int limit, int64_t from_ms) {
    std::vector<OutcomeSample> out;
    read_txn(pool_, "load_outcome_samples", [&](pqxx::work& txn) {
        // The candidate carries the identity and the market state the decision
        // was taken in; the outcome carries what it returned. Joining them is
        // what turns a trade log into evidence about a strategy.
        for (const auto& row : txn.exec_params(
                 "SELECT c.strategy_id, c.strategy_version, c.parameter_set_id, "
                 "  COALESCE(NULLIF(c.global_regime,''), '*'), "
                 "  'alts', c.canonical_pair_id, c.side, c.holding_class, "
                 "  COALESCE(o.resolved_time_ms, o.entry_time_ms, 0), o.realized_net_return "
                 "FROM altstrategy.market_outcomes o "
                 "JOIN altstrategy.strategy_candidates c ON c.candidate_id = o.candidate_id "
                 "JOIN altstrategy.trade_signal_proposals p "
                 "  ON p.signal_proposal_id = o.signal_proposal_id "
                 "WHERE NOT o.is_counterfactual AND o.outcome <> 'ENTRY_NOT_REACHED' "
                 "  AND o.realized_net_return IS NOT NULL "
                 // A trade whose stop sat 93 % away from entry is not evidence
                 // about a strategy's edge, it is evidence about a defect. Such
                 // outcomes existed here in bulk and, being four times more
                 // numerous than the sound ones, set the global prior to -34 %
                 // per trade. The estimator must never be trained on geometry
                 // no correct build could have produced; 10 % is already far
                 // wider than any intraday stop this engine plans.
                 "  AND p.stop_distance_bps > 0 AND p.stop_distance_bps <= 1000 "
                 // Learning epoch: ignore everything the previous engine produced.
                 //
                 // The casts are load-bearing. PostgreSQL infers a parameter's
                 // type from its context, and `$2 = 0` puts it next to an
                 // integer literal — so $2 was inferred as int32 and every real
                 // millisecond epoch overflowed it:
                 //
                 //   value "1785654196000" is out of range for type integer
                 //
                 // This query therefore failed on every call from the moment a
                 // learning epoch was configured, and because the transaction's
                 // error was discarded it returned an empty sample instead. The
                 // adaptive layer read that as "no history": posteriors stayed
                 // empty, every candidate carried edge 0, and no lifecycle
                 // transition could fire in either direction.
                 "  AND ($2::bigint = 0 OR p.created_at_ms >= $2::bigint) "
                 "ORDER BY o.created_at DESC LIMIT $1",
                 limit, from_ms)) {
            OutcomeSample s;
            s.strategy_id = row[0].as<std::string>();
            s.strategy_version = row[1].as<std::string>();
            s.parameter_set_id = row[2].as<std::string>();
            s.regime = row[3].as<std::string>();
            s.symbol_group = row[4].as<std::string>();
            s.canonical_pair_id = row[5].as<int64_t>();
            s.side = side_from_string(row[6].as<std::string>());
            s.holding_class = holding_from_string(row[7].as<std::string>());
            s.resolved_at_ms = row[8].as<int64_t>();
            s.net_return = row[9].as<double>();
            out.push_back(std::move(s));
        }
    });
    return out;
}

std::vector<DecisionStore::ReplayProposal> DecisionStore::load_replay_sample(int limit) {
    std::vector<ReplayProposal> out;
    read_txn(pool_, "load_replay_sample", [&](pqxx::work& txn) {
        for (const auto& row : txn.exec_params(
                 "SELECT p.signal_proposal_id,p.candidate_id,p.canonical_pair_id,p.side,p.entry_type,"
                 "p.entry_price,p.trigger_price,p.stop_loss_price,p.take_profit_price,p.created_at_ms,"
                 "p.entry_fill_timeout_ms,p.valid_until_ms,p.time_stop_at_ms,"
                 "COALESCE(NULLIF(c.global_regime,''),'*'),"
                 // ATR in price units, recovered from the geometry that was used.
                 "CASE WHEN p.stop_distance_atr > 0 "
                 "     THEN abs(p.entry_price - p.stop_loss_price)/p.stop_distance_atr END "
                 "FROM altstrategy.trade_signal_proposals p "
                 "JOIN altstrategy.strategy_candidates c ON c.candidate_id=p.candidate_id "
                 "JOIN altstrategy.market_outcomes o ON o.signal_proposal_id=p.signal_proposal_id "
                 "WHERE p.stop_distance_bps <= 1000 AND p.stop_distance_atr > 0 "
                 "ORDER BY p.created_at_ms DESC LIMIT $1",
                 limit)) {
            ReplayProposal r;
            r.p.signal_proposal_id = row[0].as<std::string>();
            r.p.candidate_id = row[1].as<std::string>();
            r.p.canonical_pair_id = row[2].as<int64_t>();
            r.p.side = side_from_string(row[3].as<std::string>());
            r.p.entry_type = entry_from_string(row[4].as<std::string>());
            r.p.entry_price = row[5].as<double>(); r.p.trigger_price = row[6].as<double>();
            r.p.stop_loss_price = row[7].as<double>(); r.p.take_profit_price = row[8].as<double>();
            r.p.created_at_ms = row[9].as<int64_t>();
            r.p.entry_fill_timeout_ms = row[10].as<int64_t>();
            r.p.valid_until_ms = row[11].as<int64_t>();
            r.p.time_stop_at_ms = row[12].as<int64_t>();
            r.regime = row[13].as<std::string>();
            if (!row[14].is_null()) r.atr_used = row[14].as<double>();
            if (r.atr_used > 0) out.push_back(std::move(r));
        }
    });
    return out;
}

bool DecisionStore::save_calibration_run(const std::string& run_id, const std::string& target_kind,
                                         const std::string& scope_json,
                                         const std::string& result_json) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.parameter_calibration_runs"
            "(run_id,strategy_id,strategy_version,target_kind,context_scope,predicted_vs_actual,status) "
            "VALUES($1,'*','*',$2,$3::jsonb,$4::jsonb,'proposed') "
            "ON CONFLICT (run_id) DO UPDATE SET predicted_vs_actual=EXCLUDED.predicted_vs_actual",
            run_id, target_kind, scope_json, result_json);
    });
    return ok("save_calibration_run", r);
}

PosteriorTable DecisionStore::load_posteriors() {
    PosteriorTable table;
    read_txn(pool_, "load_posteriors", [&](pqxx::work& txn) {
        for (const auto& row : txn.exec(
                 "SELECT strategy_id,strategy_version,parameter_set_id,regime,symbol_group,canonical_pair_id,"
                 "side,holding_class,strategy_prior,posterior_mean_edge,posterior_uncertainty,"
                 "effective_sample_size,shrinkage_factor,time_decay_state,adaptive_weight,sample_count "
                 "FROM altstrategy.strategy_posteriors")) {
            StrategyPosterior p;
            p.strategy_id = row[0].as<std::string>(); p.strategy_version = row[1].as<std::string>();
            p.parameter_set_id = row[2].as<std::string>(); p.regime = row[3].as<std::string>();
            p.symbol_group = row[4].as<std::string>(); p.canonical_pair_id = row[5].as<int64_t>();
            p.side = side_from_string(row[6].as<std::string>()); p.holding_class = holding_from_string(row[7].as<std::string>());
            p.strategy_prior = row[8].as<double>(); p.posterior_mean_edge = row[9].as<double>();
            p.posterior_uncertainty = row[10].as<double>(); p.effective_sample_size = row[11].as<double>();
            p.shrinkage_factor = row[12].as<double>(); p.time_decay_state = row[13].as<double>();
            p.adaptive_weight = row[14].as<double>(); p.sample_count = row[15].as<int64_t>();
            table.put(p);
        }
    });
    return table;
}

std::map<std::string, Lifecycle> DecisionStore::load_lifecycle_overrides() {
    std::map<std::string, Lifecycle> out;
    read_txn(pool_, "load_lifecycle_overrides", [&](pqxx::work& txn) {
        for (const auto& row : txn.exec("SELECT strategy_id,lifecycle_status FROM altstrategy.strategy_lifecycle"))
            out[row[0].as<std::string>()] = lifecycle_from_string(row[1].as<std::string>());
    });
    return out;
}

namespace {
// param_values is a flat JSON object of name -> number. A dependency-free reader
// keeps the store free of a JSON library it needs nowhere else.
std::map<std::string, double> parse_param_values(const std::string& json) {
    std::map<std::string, double> out;
    size_t i = 0;
    while (i < json.size()) {
        const size_t ks = json.find('"', i);
        if (ks == std::string::npos) break;
        const size_t ke = json.find('"', ks + 1);
        if (ke == std::string::npos) break;
        const std::string key = json.substr(ks + 1, ke - ks - 1);
        const size_t colon = json.find(':', ke);
        if (colon == std::string::npos) break;
        size_t vs = colon + 1;
        while (vs < json.size() && (json[vs] == ' ' || json[vs] == '\t')) ++vs;
        size_t ve = vs;
        while (ve < json.size() && (std::isdigit(static_cast<unsigned char>(json[ve])) ||
                                    json[ve] == '-' || json[ve] == '+' || json[ve] == '.' ||
                                    json[ve] == 'e' || json[ve] == 'E')) ++ve;
        if (ve > vs) {
            try { out[key] = std::stod(json.substr(vs, ve - vs)); } catch (...) {}
        }
        i = ve;
    }
    return out;
}
}  // namespace

bool DecisionStore::ensure_strategy_definition(const engine::StrategyMeta& m) {
    // strategy_definitions is the database's record of WHICH hypotheses a
    // release contains and what each one claims to be. The table has existed
    // since 0001 with a foreign key pointing at it from strategy_versions, and
    // nothing has ever written a row -- so the schema asserted a catalogue the
    // database could not name. Six strategies were added in one release without
    // that being visible anywhere except the binary.
    const auto text_array = [](const std::vector<std::string>& v) {
        std::string a = "{";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) a += ",";
            a += "\"" + v[i] + "\"";
        }
        return a + "}";
    };
    std::vector<std::string> sides, classes;
    for (auto side : m.supported_sides) sides.push_back(engine::to_string(side));
    for (auto hc : m.holding_classes) classes.push_back(engine::to_string(hc));

    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altstrategy.strategy_definitions(strategy_id,family,supported_sides,"
            "supported_horizons,holding_classes,allowed_regimes,forbidden_regimes,"
            "explanation_template) VALUES($1,$2,$3,$4,$5,$6,$7,$8) "
            "ON CONFLICT (strategy_id) DO UPDATE SET family=EXCLUDED.family,"
            "supported_sides=EXCLUDED.supported_sides,holding_classes=EXCLUDED.holding_classes,"
            "allowed_regimes=EXCLUDED.allowed_regimes,"
            "forbidden_regimes=EXCLUDED.forbidden_regimes,"
            "explanation_template=EXCLUDED.explanation_template",
            m.strategy_id, engine::to_string(m.family), text_array(sides),
            text_array(classes),   // horizons and holding classes are the same claim here
            text_array(classes),
            // No setup is gated on a regime any more (strategy.hpp): the
            // columns are written empty rather than dropped, so the table
            // keeps its shape for the history it already holds.
            text_array({}), text_array({}), m.explanation_template);
    });
    return ok("ensure_strategy_definition", r);
}

bool DecisionStore::ensure_parameter_schema(const std::string& strategy_id,
                                            const std::string& version,
                                            const std::vector<engine::StrategyParam>& schema) {
    if (schema.empty()) return false;
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        for (const auto& p : schema) {
            txn.exec_params(
                "INSERT INTO altstrategy.strategy_parameter_schemas(strategy_id,strategy_version,"
                "param_name,param_type,min_value,max_value,step_value,default_value) "
                "VALUES($1,$2,$3,$4,$5,$6,$7,$8) "
                "ON CONFLICT (strategy_id,strategy_version,param_name) DO UPDATE SET "
                "min_value=EXCLUDED.min_value,max_value=EXCLUDED.max_value,"
                "step_value=EXCLUDED.step_value,default_value=EXCLUDED.default_value",
                strategy_id, version, p.name, p.type, p.min, p.max, p.step, p.default_value);
        }
        // Seed an ACTIVE set from the defaults only when none exists; never
        // overwrite a calibrated one.
        std::string json = "{";
        for (size_t i = 0; i < schema.size(); ++i) {
            if (i) json += ",";
            json += "\"" + schema[i].name + "\":" + std::to_string(schema[i].default_value);
        }
        json += "}";
        txn.exec_params(
            "INSERT INTO altstrategy.strategy_parameter_sets(parameter_set_id,strategy_id,"
            "strategy_version,param_values,status,activated_at) "
            "SELECT $1,$2,$3,$4::jsonb,'active',now() WHERE NOT EXISTS ("
            "  SELECT 1 FROM altstrategy.strategy_parameter_sets"
            "  WHERE strategy_id=$2 AND status='active')",
            "ps_" + strategy_id + "_default", strategy_id, version, json);
    });
    return ok("ensure_parameter_schema", r);
}

bool DecisionStore::save_parameter_set(const std::string& strategy_id, const std::string& version,
                                       const std::map<std::string, double>& values,
                                       const std::string& derived_from, int64_t now_ms) {
    if (values.empty()) return false;
    std::string json = "{";
    bool first = true;
    for (const auto& [k, v] : values) {
        if (!first) json += ",";
        first = false;
        json += "\"" + k + "\":" + std::to_string(v);
    }
    json += "}";
    const std::string id = "ps_" + strategy_id + "_" + std::to_string(now_ms);
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        // Exactly one active set per strategy: retire the incumbent in the same
        // transaction that installs the successor.
        // 'retired' is the schema's own vocabulary for a set that has been
        // replaced; the status column carries a CHECK constraint and any other
        // word aborts the transaction — which this function then reported as a
        // plain false, so the calibrator failed silently for every strategy.
        txn.exec_params(
            "UPDATE altstrategy.strategy_parameter_sets SET status='retired' "
            "WHERE strategy_id=$1 AND status='active'",
            strategy_id);
        txn.exec_params(
            "INSERT INTO altstrategy.strategy_parameter_sets(parameter_set_id,strategy_id,"
            "strategy_version,param_values,status,derived_from,activated_at) "
            "VALUES($1,$2,$3,$4::jsonb,'active',$5,now()) "
            "ON CONFLICT (parameter_set_id) DO NOTHING",
            id, strategy_id, version, json, derived_from);
    });
    if (!r) {
        log::warn("parameter_set_write_failed",
                  {{"strategy", strategy_id}, {"message", r.error().message}});
        return false;
    }
    return true;
}

std::map<std::string, engine::CalibrationTargets>
DecisionStore::load_calibration_targets(int64_t from_ms) {
    std::map<std::string, engine::CalibrationTargets> out;
    read_txn(pool_, "save_parameter_set", [&](pqxx::work& txn) {
        // Predicted comes from what the plan promised, actual from what the
        // reconstructed path delivered. Only filled, sane-geometry trades count:
        // an unfilled entry measures the entry offset, not the parameters.
        for (const auto& row : txn.exec_params(
                 "SELECT p.strategy_id || '|' || COALESCE(p.parameter_set_id,''),"
                 // Both MAE terms must be in the SAME unit or their ratio is meaningless:
                 // the planned stop distance and the realised adverse excursion, both
                 // in bps. Mixing ATR multiples with a fraction-of-stop was a units bug.
                 " avg(p.stop_distance_bps), avg(o.mae * 10000),"
                 " avg(p.target_distance_bps), avg(o.mfe * 10000),"
                 " avg(p.expected_fill_time_p50_min), avg((o.entry_time_ms - p.created_at_ms)/60000.0),"
                 " avg(p.expected_holding_time_p50_ms/60000.0), avg(o.holding_time_ms/60000.0),"
                 " count(*) "
                 "FROM altstrategy.trade_signal_proposals p "
                 "JOIN altstrategy.market_outcomes o ON o.signal_proposal_id=p.signal_proposal_id "
                 " AND NOT o.is_counterfactual AND o.outcome <> 'ENTRY_NOT_REACHED' "
                 "WHERE p.created_at_ms >= $1 AND p.stop_distance_bps > 0 "
                 " AND p.stop_distance_bps <= 1000 AND p.strategy_id <> '' "
                 "GROUP BY 1",
                 from_ms)) {
            engine::CalibrationTargets t;
            const auto num = [&](int i) { return row[i].is_null() ? 0.0 : row[i].as<double>(); };
            t.predicted_mae = num(1);   t.actual_mae = num(2);
            t.predicted_mfe = num(3);   t.actual_mfe = num(4);
            t.predicted_fill_min = num(5);    t.actual_fill_min = num(6);
            t.predicted_holding_min = num(7); t.actual_holding_min = num(8);
            t.sample_count = row[9].as<int64_t>();
            out[row[0].as<std::string>()] = t;
        }
    });
    return out;
}

std::map<std::string, ParamValues> DecisionStore::load_param_sets() {
    std::map<std::string, ParamValues> out;
    read_txn(pool_, "load_param_sets", [&](pqxx::work& txn) {
        for (const auto& row : txn.exec(
                 // Keyed off the SET, not off strategy_versions.default_parameter_set_id:
                 // a calibrated set is a NEW active row, and pinning the join to the
                 // default pointer meant a freshly calibrated set was never seen.
                 "SELECT ps.strategy_id,ps.parameter_set_id,COALESCE(ps.param_values::text,'{}') "
                 "FROM altstrategy.strategy_parameter_sets ps WHERE ps.status='active'")) {
            ParamValues p;
            p.parameter_set_id = row[1].as<std::string>();
            // The values themselves were never decoded — only the id was carried and
            // the engine silently fell back to compiled defaults, so a calibrated
            // set could be stored and loaded and still change nothing.
            p.values = parse_param_values(row[2].as<std::string>());
            out[row[0].as<std::string>()] = std::move(p);
        }
    });
    return out;
}

std::map<std::string, std::pair<std::string, engine::DriftInputs>>
DecisionStore::load_drift_inputs(int64_t from_ms, int64_t split_ms) {
    std::map<std::string, std::pair<std::string, engine::DriftInputs>> out;
    // Candidate frequency is normalised by the number of decision contexts each
    // window offered, not by elapsed time — see the `ctx` CTE below. Elapsed
    // hours are no longer part of the measure: after the upstream outage the
    // per-hour rate differed by roughly an order of magnitude between the two
    // windows, which read as drift in every strategy at once when it was only a
    // statement about how long AltTech had been down.
    read_txn(pool_, "load_param_sets", [&](pqxx::work& txn) {
        // Regime and symbol performance are MACRO-averages: the mean net return
        // is taken per regime (per pair) first and those means are then averaged
        // with equal weight. Pooling instead would let whichever regime happened
        // to trade most in one window masquerade as a change in behaviour.
        const auto res = txn.exec_params(
            "WITH w AS ("
            "  SELECT p.strategy_id, p.strategy_version, (p.created_at_ms >= $2) AS recent,"
            "         c.adaptive_score, c.bitget_regime, p.canonical_pair_id,"
            "         o.realized_net_return AS net, o.mfe, o.mae, o.holding_time_ms,"
            "         (o.outcome IS NOT NULL) AS resolved,"
            "         (o.outcome IS NOT NULL AND o.outcome <> 'ENTRY_NOT_REACHED') AS filled"
            "  FROM altstrategy.trade_signal_proposals p"
            "  LEFT JOIN altstrategy.strategy_candidates c ON c.candidate_id = p.candidate_id"
            "  LEFT JOIN altstrategy.market_outcomes o"
            "    ON o.signal_proposal_id = p.signal_proposal_id AND NOT o.is_counterfactual"
            "  WHERE p.created_at_ms >= $1 AND p.proposal_grade <> 'INVALID'"
            "    AND p.stop_distance_bps > 0 AND p.stop_distance_bps <= 1000),"
            "reg AS (SELECT strategy_id, recent, avg(m) v,"
            "               coalesce(stddev_samp(m),0) sd, count(*) k FROM ("
            "  SELECT strategy_id, recent, bitget_regime, avg(net) m FROM w"
            "  WHERE filled AND bitget_regime <> '' GROUP BY 1,2,3) t GROUP BY 1,2),"
            "sym AS (SELECT strategy_id, recent, avg(m) v,"
            "               coalesce(stddev_samp(m),0) sd, count(*) k FROM ("
            "  SELECT strategy_id, recent, canonical_pair_id, avg(net) m FROM w"
            "  WHERE filled GROUP BY 1,2,3) t GROUP BY 1,2),"
            // How many chances to act the engine actually had in each window.
            // Candidate frequency is normalised by this rather than by elapsed
            // hours: a strategy that fires on every context it is given has not
            // changed behaviour just because upstream stopped supplying them.
            "ctx AS (SELECT (event_time_ms >= $2) AS recent, count(*)::float8 n"
            "  FROM altstrategy.decision_context_snapshots"
            "  WHERE event_time_ms >= $1 GROUP BY 1) "
            "SELECT w.strategy_id, max(w.strategy_version),"
            "  count(*) FILTER (WHERE NOT w.recent)::float8 "
            "    / GREATEST(COALESCE(max(cb.n), 0), 1),"
            "  count(*) FILTER (WHERE w.recent)::float8 "
            "    / GREATEST(COALESCE(max(cr.n), 0), 1),"
            "  COALESCE(avg(w.adaptive_score) FILTER (WHERE NOT w.recent),0),"
            "  COALESCE(avg(w.adaptive_score) FILTER (WHERE w.recent),0),"
            "  COALESCE(avg((w.net > 0)::int::float8) FILTER (WHERE NOT w.recent AND w.filled),0),"
            "  COALESCE(avg((w.net > 0)::int::float8) FILTER (WHERE w.recent AND w.filled),0),"
            "  COALESCE(avg(w.mfe) FILTER (WHERE NOT w.recent AND w.filled),0),"
            "  COALESCE(avg(w.mfe) FILTER (WHERE w.recent AND w.filled),0),"
            "  COALESCE(avg(w.mae) FILTER (WHERE NOT w.recent AND w.filled),0),"
            "  COALESCE(avg(w.mae) FILTER (WHERE w.recent AND w.filled),0),"
            "  COALESCE(avg(w.holding_time_ms) FILTER (WHERE NOT w.recent AND w.filled),0),"
            "  COALESCE(avg(w.holding_time_ms) FILTER (WHERE w.recent AND w.filled),0),"
            "  COALESCE(avg(w.filled::int::float8) FILTER (WHERE NOT w.recent AND w.resolved),0),"
            "  COALESCE(avg(w.filled::int::float8) FILTER (WHERE w.recent AND w.resolved),0),"
            "  COALESCE(max(rb.v),0), COALESCE(max(rr.v),0),"
            "  COALESCE(max(sb.v),0), COALESCE(max(sr.v),0),"
            "  count(*) FILTER (WHERE w.filled), "
            "  count(*) FILTER (WHERE w.filled AND NOT w.recent), "
            "  count(*) FILTER (WHERE w.filled AND w.recent), "
            // Dispersion and group count for the two macro-averages, so their
            // drift can be judged against uncertainty rather than magnitude.
            "  COALESCE(max(rb.sd),0), COALESCE(max(rr.sd),0), "
            "  COALESCE(max(sb.sd),0), COALESCE(max(sr.sd),0), "
            "  COALESCE(max(rb.k),0), COALESCE(max(rr.k),0), "
            "  COALESCE(max(sb.k),0), COALESCE(max(sr.k),0) "
            "FROM w "
            "LEFT JOIN reg rb ON rb.strategy_id=w.strategy_id AND NOT rb.recent "
            "LEFT JOIN reg rr ON rr.strategy_id=w.strategy_id AND rr.recent "
            "LEFT JOIN sym sb ON sb.strategy_id=w.strategy_id AND NOT sb.recent "
            "LEFT JOIN sym sr ON sr.strategy_id=w.strategy_id AND sr.recent "
            "LEFT JOIN ctx cb ON NOT cb.recent "
            "LEFT JOIN ctx cr ON cr.recent "
            "GROUP BY w.strategy_id",
            from_ms, split_ms);
        for (const auto& row : res) {
            engine::DriftInputs in;
            in.baseline_candidate_freq = row[2].as<double>();
            in.recent_candidate_freq = row[3].as<double>();
            in.baseline_score_mean = row[4].as<double>();
            in.recent_score_mean = row[5].as<double>();
            in.baseline_win_rate = row[6].as<double>();
            in.recent_win_rate = row[7].as<double>();
            in.baseline_mfe = row[8].as<double>();  in.recent_mfe = row[9].as<double>();
            in.baseline_mae = row[10].as<double>(); in.recent_mae = row[11].as<double>();
            in.baseline_holding = row[12].as<double>(); in.recent_holding = row[13].as<double>();
            in.baseline_fill = row[14].as<double>();    in.recent_fill = row[15].as<double>();
            in.baseline_regime_perf = row[16].as<double>();
            in.recent_regime_perf = row[17].as<double>();
            in.baseline_symbol_perf = row[18].as<double>();
            in.recent_symbol_perf = row[19].as<double>();
            in.sample_count = row[20].as<int64_t>();
            in.baseline_sample_count = row[21].as<int64_t>();
            in.recent_sample_count = row[22].as<int64_t>();
            in.baseline_regime_perf_sd = row[23].as<double>();
            in.recent_regime_perf_sd = row[24].as<double>();
            in.baseline_symbol_perf_sd = row[25].as<double>();
            in.recent_symbol_perf_sd = row[26].as<double>();
            in.baseline_regime_groups = row[27].as<int64_t>();
            in.recent_regime_groups = row[28].as<int64_t>();
            in.baseline_symbol_groups = row[29].as<int64_t>();
            in.recent_symbol_groups = row[30].as<int64_t>();
            out[row[0].as<std::string>()] = {row[1].as<std::string>(), in};
        }
    });
    return out;
}

std::vector<AttributionWork> DecisionStore::load_unattributed(int limit) {
    std::vector<AttributionWork> out;
    read_txn(pool_, "load_unattributed", [&](pqxx::work& txn) {
        // Pick candidates that have at least one resolved-but-unattributed
        // variant, then pull ALL of that candidate's variants in the same read —
        // variant regret is only meaningful against its siblings.
        for (const auto& row : txn.exec_params(
                 "WITH todo AS ("
                 "  SELECT DISTINCT p.candidate_id FROM altstrategy.trade_signal_proposals p"
                 "  JOIN altstrategy.market_outcomes o"
                 "    ON o.signal_proposal_id=p.signal_proposal_id AND NOT o.is_counterfactual"
                 "  LEFT JOIN altstrategy.outcome_attribution a"
                 "    ON a.signal_proposal_id=p.signal_proposal_id"
                 "  WHERE a.attribution_id IS NULL"
                 // Same geometry guard the posteriors use: an outcome produced
                 // under the old broken stop geometry is not a measurement of
                 // anything, and attributing it only launders it into analysis.
                 "    AND p.stop_distance_bps > 0 AND p.stop_distance_bps <= 1000"
                 "    AND o.outcome <> 'ENTRY_NOT_REACHED' LIMIT $1) "
                 "SELECT p.candidate_id,COALESCE(p.ranking_id,''),p.canonical_pair_id,p.plan_version,"
                 "p.signal_proposal_id,COALESCE(p.round_trip_cost_bps,0),"
                 "COALESCE(p.expected_net_return,0),(a.attribution_id IS NULL),"
                 "COALESCE(p.edge_estimated,false),"
                 "o.outcome,o.realized_gross_return,o.realized_net_return,o.mfe,o.mae,"
                 "o.entry_fill_price,o.resolved_time_ms,COALESCE(sc.bitget_regime,''),"
                 "COALESCE(rr.bitget_regime,'') "
                 "FROM todo t "
                 "JOIN altstrategy.trade_signal_proposals p ON p.candidate_id=t.candidate_id "
                 "JOIN altstrategy.market_outcomes o"
                 "  ON o.signal_proposal_id=p.signal_proposal_id AND NOT o.is_counterfactual "
                 "  AND o.outcome <> 'ENTRY_NOT_REACHED' "
                 "LEFT JOIN altstrategy.outcome_attribution a"
                 "  ON a.signal_proposal_id=p.signal_proposal_id "
                 "LEFT JOIN altstrategy.strategy_candidates sc ON sc.candidate_id=p.candidate_id "
                 "LEFT JOIN LATERAL ("
                 "  SELECT sc2.bitget_regime FROM altstrategy.strategy_candidates sc2"
                 "  WHERE sc2.canonical_pair_id=p.canonical_pair_id"
                 "    AND sc2.created_at_ms<=o.resolved_time_ms"
                 "  ORDER BY sc2.created_at_ms DESC LIMIT 1) rr ON TRUE "
                 "ORDER BY p.candidate_id,p.plan_version",
                 limit)) {
            const auto cid = row[0].as<std::string>();
            if (out.empty() || out.back().candidate_id != cid) {
                AttributionWork w;
                w.candidate_id = cid; w.ranking_id = row[1].as<std::string>();
                w.canonical_pair_id = row[2].as<int64_t>();
                w.regime_at_creation = row[16].as<std::string>();
                w.regime_at_resolution = row[17].as<std::string>();
                out.push_back(std::move(w));
            }
            VariantOutcome v;
            v.plan_version = row[3].as<int>();
            v.signal_proposal_id = row[4].as<std::string>();
            v.round_trip_cost_bps = row[5].as<double>();
            v.expected_net_return = row[6].as<double>();
            v.needs_attribution = row[7].as<bool>();
            v.edge_estimated = row[8].as<bool>();
            v.outcome.signal_proposal_id = v.signal_proposal_id;
            v.outcome.candidate_id = cid;
            v.outcome.canonical_pair_id = out.back().canonical_pair_id;
            v.outcome.outcome = market_outcome_from_string(row[9].as<std::string>());
            v.outcome.realized_gross_return = row[10].as<double>();
            v.outcome.realized_net_return = row[11].as<double>();
            v.outcome.mfe = row[12].as<double>(); v.outcome.mae = row[13].as<double>();
            v.outcome.entry_fill_price = row[14].as<double>();
            v.outcome.resolved_time_ms = row[15].as<int64_t>();
            out.back().variants.push_back(std::move(v));
        }
    });
    return out;
}

std::vector<PendingProposal> DecisionStore::proposals_awaiting_outcome(int64_t now_ms, int limit) {
    std::vector<PendingProposal> out;
    read_txn(pool_, "proposals_awaiting_outcome", [&](pqxx::work& txn) {
        for (const auto& row : txn.exec_params(
                 "SELECT p.signal_proposal_id,p.candidate_id,p.canonical_pair_id,p.side,p.entry_type,"
                 "p.entry_price,p.trigger_price,p.stop_loss_price,p.take_profit_price,p.created_at_ms,"
                 "p.entry_fill_timeout_ms,p.valid_until_ms,p.time_stop_at_ms,p.proposal_grade,"
                 "COALESCE(p.ranking_id,''),COALESCE(p.strategy_id,''),COALESCE(p.strategy_version,''),"
                 "COALESCE(p.edge_estimated,false),COALESCE(p.expected_net_return,0),"
                 "COALESCE(sc.bitget_regime,''),"
                 // The cost this plan was PRICED with, so it is scored with the
                 // same one. NULL means the row predates cost model v2, and the
                 // legacy flat 8.0 is what was applied to it -- stated here
                 // rather than left to a struct default nobody could see.
                 "COALESCE(p.round_trip_cost_bps, 8.0),COALESCE(p.cost_model_version, 1) "
                 "FROM altstrategy.trade_signal_proposals p "
                 "LEFT JOIN altstrategy.market_outcomes o ON o.signal_proposal_id=p.signal_proposal_id "
                 "LEFT JOIN altstrategy.strategy_candidates sc ON sc.candidate_id=p.candidate_id "
                 "WHERE o.outcome_id IS NULL AND p.proposal_grade<>'INVALID' AND p.time_stop_at_ms < $1 "
                 "LIMIT $2",
                 now_ms, limit)) {
            PendingProposal pp;
            pp.signal_proposal_id = row[0].as<std::string>(); pp.candidate_id = row[1].as<std::string>();
            pp.canonical_pair_id = row[2].as<int64_t>(); pp.side = side_from_string(row[3].as<std::string>());
            pp.entry_type = entry_from_string(row[4].as<std::string>()); pp.entry_price = row[5].as<double>();
            pp.trigger_price = row[6].as<double>(); pp.stop_loss_price = row[7].as<double>();
            pp.take_profit_price = row[8].as<double>(); pp.created_at_ms = row[9].as<int64_t>();
            pp.entry_fill_timeout_ms = row[10].as<int64_t>(); pp.valid_until_ms = row[11].as<int64_t>();
            pp.time_stop_at_ms = row[12].as<int64_t>(); pp.grade = grade_from_string(row[13].as<std::string>());
            pp.ranking_id = row[14].as<std::string>(); pp.strategy_id = row[15].as<std::string>();
            pp.strategy_version = row[16].as<std::string>();
            pp.edge_estimated = row[17].as<bool>(); pp.expected_net_return = row[18].as<double>();
            pp.regime_at_creation = row[19].as<std::string>();
            pp.cost_bps = row[20].as<double>();
            pp.cost_model_version = row[21].as<int>();
            out.push_back(std::move(pp));
        }
    });
    return out;
}

bool DecisionStore::mark_proposal_state(const std::string& id, ProposalState state,
                                        const std::string& reason, int64_t at_ms) {
    auto r = pool_.with_txn([&](pqxx::work& txn) {
        txn.exec_params("UPDATE altstrategy.trade_signal_proposals SET state=$2 WHERE signal_proposal_id=$1",
                        id, to_string(state));
        // A state event records a TRANSITION. Re-entering the state the proposal
        // is already in is not one, and appending it anyway turned the audit
        // trail into its own hot table. Record the transition, and carry the
        // state it came from so the trail is readable.
        txn.exec_params(
            "WITH last AS ("
            "  SELECT ordinal, to_state FROM altstrategy.proposal_state_events "
            "  WHERE signal_proposal_id = $1 ORDER BY ordinal DESC LIMIT 1) "
            "INSERT INTO altstrategy.proposal_state_events"
            "(signal_proposal_id,ordinal,from_state,to_state,reason,at_ms) "
            "SELECT $1, COALESCE((SELECT ordinal FROM last), 0) + 1, "
            "       (SELECT to_state FROM last), $2, $3, $4 "
            "WHERE (SELECT to_state FROM last) IS DISTINCT FROM $2",
            id, to_string(state), reason, at_ms);
    });
    return ok("mark_proposal_state", r);
}

std::vector<DecisionStore::RetentionResult> DecisionStore::run_retention(int64_t now_ms) {
    struct Policy { std::string table; int retain_days; std::string time_column; };
    std::vector<Policy> policies;
    read_txn(pool_, "run_retention", [&](pqxx::work& txn) {
        for (const auto& row : txn.exec(
                 "SELECT table_name, retain_days, time_column FROM altstrategy.retention_policies "
                 "ORDER BY table_name")) {
            policies.push_back({row[0].as<std::string>(), row[1].as<int>(), row[2].as<std::string>()});
        }
    });

    std::vector<RetentionResult> results;
    results.reserve(policies.size());
    for (const auto& p : policies) {
        RetentionResult res{p.table, 0, true, {}};
        int64_t deleted = 0;
        // table_name / time_column come only from our own retention_policies rows
        // (never external input), but quote_name() is used regardless — belt and
        // braces against SQL injection (OWASP A03).
        auto del = pool_.with_txn([&](pqxx::work& txn) {
            const std::string q = "DELETE FROM altstrategy." + txn.quote_name(p.table) +
                " WHERE " + txn.quote_name(p.time_column) +
                " < now() - make_interval(days => " + std::to_string(p.retain_days) + ")";
            auto r = txn.exec(q);
            deleted = static_cast<int64_t>(r.affected_rows());
        });
        if (!del.has_value()) {
            res.ok = false;
            res.error = del.error().message;   // carried, not swallowed
            results.push_back(res);
            continue;
        }
        res.deleted_rows = deleted;
        results.push_back(res);
        if (deleted > 0) {
            read_txn(pool_, "run_retention", [&](pqxx::work& txn) {
                txn.exec_params(
                    "INSERT INTO altstrategy.retention_runs(run_id, table_name, deleted_rows) "
                    "VALUES($1,$2,$3)",
                    p.table + "_" + std::to_string(now_ms), p.table, deleted);
            });
        }
    }
    return results;
}

} // namespace altstrategy::engine_io
