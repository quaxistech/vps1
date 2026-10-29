#include "altstrategy/engine/proposal.hpp"
#include "altstrategy/engine/cost_model.hpp"

#include "altstrategy/engine/ids.hpp"
#include "altstrategy/engine/units.hpp"

#include <algorithm>
#include <cstdio>
#include <cmath>

namespace altstrategy::engine {

// Effective sample size at which a strategy cell's posterior edge counts as
// measured (reported as a diagnostic). Matches AllocatorConfig::prior_strength:
// at this ESS the shrinkage weight on the local estimate is exactly one half.
constexpr double kMinEssMeasured = 20.0;

namespace {
bool bad(double x) { return !std::isfinite(x); }
// One definition of the ceiling, in decision_types.hpp.
constexpr int64_t k48h = kMaxHoldingMs;

// Warning thresholds. Named so a proposal's warning can state the bound it
// crossed, and so the bound has one definition rather than a literal at the
// comparison site. These are OPERATIONAL alarm levels, not tuned parameters:
// none has been fitted, and each is a round number chosen to be noticed.
constexpr double kMinCrossVenueAgreement = 0.2;
constexpr double kWideSpreadBps = 8.0;        // ~half the 16 bps round trip
constexpr double kElevatedSlippageBps = 8.0;
constexpr double kMinEffectiveSamples = 20.0;
constexpr double kHighFundingRate = 0.0008;   // 0.08% per 8h
}

std::vector<std::string> invalid_reasons(const TradePlanVariant& p, const DecisionContext& ctx,
                                         int64_t now_ms) {
    std::vector<std::string> r;
    // Universe / pair validity.
    if (ctx.global_universe_status.rfind("EXCLUDED", 0) == 0 ||
        ctx.global_universe_status.rfind("MISSING", 0) == 0)
        r.push_back("PAIR_EXCLUDED_FROM_UNIVERSE");
    if (ctx.data_quality == ContextQuality::invalid) r.push_back("CONTRACT_QUALITY_INVALID");
    if (ctx.feature_manifest_hash.empty()) r.push_back("FEATURE_MANIFEST_MISSING");
    // Expired context.
    if (ctx.expires_at_ms > 0 && now_ms > ctx.expires_at_ms) r.push_back("EXPIRED_CONTEXT");
    // NaN/Inf.
    if (bad(p.entry_price) || bad(p.stop_loss_price) || bad(p.take_profit_price))
        r.push_back("NAN_OR_INF_PRICE");
    if (p.entry_price <= 0 || p.stop_loss_price <= 0 || p.take_profit_price <= 0)
        r.push_back("NON_POSITIVE_PRICE");
    // Price-side invariants.
    if (p.side == Side::long_) {
        if (p.take_profit_price <= p.entry_price) r.push_back("LONG_TP_LE_ENTRY");
        if (p.stop_loss_price >= p.entry_price) r.push_back("LONG_SL_GE_ENTRY");
    } else if (p.side == Side::short_) {
        if (p.take_profit_price >= p.entry_price) r.push_back("SHORT_TP_GE_ENTRY");
        if (p.stop_loss_price <= p.entry_price) r.push_back("SHORT_SL_LE_ENTRY");
    }
    // Non-positive distances.
    if (std::abs(p.entry_price - p.stop_loss_price) <= 0) r.push_back("ZERO_STOP_DISTANCE");
    if (std::abs(p.take_profit_price - p.entry_price) <= 0) r.push_back("ZERO_TARGET_DISTANCE");
    // Holding limit.
    if (p.max_holding_time_ms < 5LL * 60 * 1000) r.push_back("HOLDING_TIME_BELOW_MINIMUM");
    if (p.max_holding_time_ms > k48h) r.push_back("HOLDING_TIME_EXCEEDS_LIMIT");
    // Time ordering: created < time_stop; entry timeout positive; time_stop future.
    if (p.time_stop_at_ms <= now_ms) r.push_back("INVALID_TIME_ORDERING");
    if (p.entry_fill_timeout_ms <= 0) r.push_back("INVALID_ENTRY_TIMEOUT");
    if (p.entry_fill_timeout_ms > p.max_holding_time_ms) r.push_back("ENTRY_TIMEOUT_EXCEEDS_HOLDING");
    return r;
}

Grade assign_grade(const StrategyCandidate& c, const MlEvaluation& ml, const EnsembleView& ensemble,
                   bool invalid) {
    if (invalid) return Grade::invalid;
    // A VPS1 signal's grade states how COMPLETE its market view is, and
    // nothing about whether it pays -- that is VPS2's question.
    //
    //   NORMAL  an ACTIVE model's fresh, complete prediction exists for the
    //           horizon matching the setup's own
    //   WEAK    it does not (missing, stale, incomplete, abstained, or only a
    //           SHADOW model answered): the signal is still published, and the
    //           prediction block says exactly which
    //
    // It used to be the economic verdict (NORMAL = "the model says it pays")
    // and the strategy's lifecycle (SHADOW / EXPERIMENTAL for a strategy whose
    // realised PnL was negative). Both were execution judgements made in VPS1,
    // and the second demoted a detector for being a bad trader -- which says
    // nothing about whether the event it detects is informative.
    (void)ml;
    (void)ensemble;
    for (const auto& h : c.predictions) {
        if (h.horizon_minutes == c.econ.horizon_minutes)
            return h.state == PredictionState::present && h.role == "ACTIVE" ? Grade::normal : Grade::weak;
    }
    return Grade::weak;
}

TradeSignalProposal build_proposal(const ProposalInputs& in, int64_t now_ms) {
    const auto& c = in.candidate;
    const auto& p = in.plan;
    const auto& ml = in.ml;
    const bool valid_holding = p.max_holding_time_ms >= 5LL * 60 * 1000 &&
                               p.max_holding_time_ms <= k48h;
    TradeSignalProposal s;
    s.natural_key = ids::join({c.candidate_id, std::to_string(in.plan_version)});
    s.signal_proposal_id = ids::make("sig", s.natural_key);
    s.candidate_id = c.candidate_id;
    s.ranking_id = in.ranking_id;
    s.canonical_pair_id = c.canonical_pair_id;
    s.bitget_instrument_id = in.ctx.bitget_instrument_id;
    s.strategy_id = c.strategy_id;
    s.strategy_version = c.strategy_version;
    s.parameter_set_id = c.parameter_set_id;
    s.side = c.side;
    s.plan_version = in.plan_version;
    s.holding_class = c.holding_class;
    s.created_at_ms = now_ms;

    // Geometry from the plan.
    s.entry_type = p.entry_type;
    s.entry_price = p.entry_price;
    s.entry_lower = p.entry_lower;
    s.entry_upper = p.entry_upper;
    s.trigger_price = p.trigger_price;
    s.max_slippage_bps = p.max_slippage_bps;
    s.post_only_allowed = p.post_only_allowed;
    s.expected_fill_time_p50_min = p.expected_fill_time_p50_min;
    s.expected_fill_time_p90_min = p.expected_fill_time_p90_min;
    s.entry_fill_timeout_ms = p.entry_fill_timeout_ms;
    s.stop_loss_price = p.stop_loss_price;
    s.stop_basis = p.stop_basis;
    s.stop_distance_bps = p.stop_distance_bps;
    s.stop_distance_atr = p.stop_distance_atr;
    s.take_profit_price = p.take_profit_price;
    s.optional_secondary_target = p.optional_secondary_target;
    s.target_basis = p.target_basis;
    s.target_distance_bps = p.target_distance_bps;
    s.plan_geometry_version = p.geometry_version;
    s.max_holding_time_ms = p.max_holding_time_ms;
    s.time_stop_at_ms = p.time_stop_at_ms;
    s.expected_rr = p.expected_rr;

    // Signal validity: the market idea stays actionable for a slice of the
    // horizon, always shorter than the max holding (§14).
    const int64_t validity = !valid_holding ? 0 :
        std::clamp<int64_t>(c.expected_horizon_ms / 4, 5LL * 60 * 1000,
                           std::min<int64_t>(p.max_holding_time_ms, 2LL * 3600 * 1000));
    s.valid_until_ms = now_ms + validity;

    // Forward-looking economics come from AltML or they do not exist.
    //
    // The cost is measured either way and is recorded either way -- it is a
    // property of the venue and the plan, not of any forecast. What used to sit
    // in the else-branch was a set of invented probabilities feeding an
    // expected return that is identically zero by construction (see
    // TradeSignalProposal::edge_estimated). A proposal with no model now says
    // so, and downstream can tell the difference between "the edge is zero" and
    // "nobody estimated the edge".
    const auto cost = compute_trade_cost(p.entry_type, in.ctx);
    s.cost_model_version = cost.version;
    s.fee_bps_round_trip = cost.fee_bps_round_trip;
    s.spread_cost_bps = cost.spread_cost_bps;
    s.slippage_cost_bps = cost.slippage_cost_bps;
    s.round_trip_cost_bps = cost.round_trip_bps;
    s.cost_spread_observed = cost.spread_observed;

    if (c.econ.evaluated()) {
        // Only what AltML actually computes: the calibrated expected NET
        // return of this side over the model horizon -- the market outcome of
        // a taker entry held for the horizon, cost included. It is not a
        // forecast of this plan's execution (a limit entry, a stop, a target),
        // and p_target_before_stop is not produced by it at all: absent, not 0.
        s.edge_estimated = true;
        s.p_target_estimated = false;
        s.expected_net_return = units::bps_to_fraction(c.econ.expected_net_bps);
        s.ml_conviction = std::clamp(c.econ.p_side - c.econ.p_opposite, -1.0, 1.0);
        const int64_t hold = static_cast<int64_t>(c.econ.horizon_minutes) * 60'000;
        s.expected_holding_time_p50_ms = valid_holding ?
            std::clamp<int64_t>(hold, 60'000, p.max_holding_time_ms) : 0;
        s.expected_holding_time_p90_ms = valid_holding ?
            std::min<int64_t>(hold, p.max_holding_time_ms) : 0;
        if (!c.econ.model_id.empty()) s.model_versions.push_back(c.econ.model_id);
    } else {
        // No model, so no edge estimate. The holding times below come from the
        // strategy's own declared horizon, which IS produced by something.
        s.edge_estimated = false;
        s.expected_holding_time_p50_ms = valid_holding ?
            std::min<int64_t>(c.expected_horizon_ms / 2, p.max_holding_time_ms) : 0;
        s.expected_holding_time_p90_ms = valid_holding ?
            std::min<int64_t>(c.expected_horizon_ms, p.max_holding_time_ms) : 0;
    }

    // The signal's content: the market view on every horizon, the setup's
    // measured description, the horizon that matches the setup's own.
    s.market_predictions = c.predictions;
    s.setup_descriptors = c.descriptors;
    s.evaluation_horizon_minutes = c.econ.horizon_minutes;
    for (const auto& h : c.predictions) {
        if (h.has_values() && !h.model_id.empty() &&
            std::find(s.model_versions.begin(), s.model_versions.end(), h.model_id) == s.model_versions.end())
            s.model_versions.push_back(h.model_id);
    }

    s.strategy_confidence = c.adaptive_score;
    s.ensemble_agreement = in.ensemble.ensemble_agreement;
    s.adaptive_score = c.adaptive_score;
    s.quality = in.ctx.data_quality;
    s.reason_codes = c.reason_codes;
    s.context_revision = in.ctx.context_revision;
    s.feature_manifest_hash = in.ctx.feature_manifest_hash;
    s.ranking_policy_id = "champion_ml_v2";
    s.alternative_plan_ids = in.alternative_plan_ids;
    s.state = ProposalState::proposed;

    // Warnings + risk flags (never hard rejects, §13).
    const auto warn = [&](const char* code, const char* detail, const char* sev) {
        s.warnings.push_back(ProposalWarning{code, detail, sev});
    };
    if (!s.edge_estimated)
        warn("NO_EDGE_ESTIMATE",
             "no model produced an expected return; only the measured cost is known",
             "warning");
    // The economic DIAGNOSTIC, stated by name at severity "info": what
    // AltML's cost model says the expected move is worth after costs at T0.
    // It gates nothing here; VPS2 prices the signal against its own costs.
    // The condition of the PREDICTION (missing / stale / incomplete / shadow)
    // is a statement about the signal's completeness: "warning".
    (void)ml;
    {
        const std::string h = std::to_string(c.econ.horizon_minutes) + "m";
        const auto num1 = [](double v) { char b[32]; std::snprintf(b, sizeof(b), "%.1f", v); return std::string(b); };
        switch (c.econ.state) {
            case EconState::positive_edge:
            case EconState::strong_edge:
            case EconState::small_edge:
            case EconState::no_edge:
                warn(("ECON_DIAG_" + to_string(c.econ.state)).c_str(),
                     ("diagnostic: expected net " + num1(c.econ.expected_net_bps) + " bps over " + h +
                      " after " + num1(c.econ.round_trip_cost_bps) + " bps estimated cost").c_str(),
                     "info");
                break;
            case EconState::horizon_conflict:
                warn("ECON_DIAG_HORIZON_CONFLICT", ("diagnostic: another horizon's estimate favours the "
                                                    "opposite side (" + h + " evaluated)").c_str(), "info");
                break;
            case EconState::model_unavailable:
                warn("MODEL_UNAVAILABLE",
                     ("no usable prediction for the " + h + " horizon" +
                      (c.econ.reason_code == "MODEL_INCOMPATIBLE" ? std::string(" (incompatible)")
                                                                  : std::string())).c_str(),
                     "warning");
                break;
            case EconState::model_stale:
                warn("MODEL_STALE", ("prediction for " + h + " older than the freshness bound").c_str(), "warning");
                break;
            case EconState::model_abstain:
                warn("MODEL_ABSTAIN", "AltML abstained", "warning");
                s.ml_abstain = true;
                break;
            case EconState::model_shadow:
                warn("MODEL_SHADOW", ("only a SHADOW model (not past AltML's quality gates) predicted the " +
                                      h + " horizon").c_str(), "warning");
                break;
            case EconState::no_data:
                warn("MODEL_INCOMPLETE_INPUT", ("the " + h + " prediction was scored with aligned inputs "
                                                "missing").c_str(), "warning");
                break;
        }
    }
    // Each warning states the OBSERVED value against the THRESHOLD that fired
    // it. "wide spread" told an operator nothing: not how wide, not against
    // what bound, and not whether the reading was measured or a stand-in.
    const auto num = [](double v, int dp = 2) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.*f", dp, v);
        return std::string(buf);
    };
    const auto warn_over = [&](const char* code, const char* what, double observed,
                               double threshold, const char* unit, const char* sev) {
        warn(code, (std::string(what) + ": " + num(observed) + unit + " > threshold " +
                    num(threshold) + unit).c_str(), sev);
    };

    // QUALITY-03: whether the plan's own target can pay the cost it was priced
    // with. A hypothesis is not refused for it -- the market event stands and
    // is published -- but a plan that loses money even at its target is never
    // left looking viable: TARGET_FIRST on such a plan is a net loss by
    // construction (798 of 819 Donchian TARGET_FIRST outcomes, 2026-10-13).
    if (!(p.target_distance_bps > s.round_trip_cost_bps))
        warn("PLAN_TARGET_BELOW_COST",
             ("diagnostic: target " + num(p.target_distance_bps, 1) + " bps (" + p.target_basis +
              ") <= estimated round trip " + num(s.round_trip_cost_bps, 1) + " bps").c_str(),
             "info");
    // DESIGN-02: a grid guessed from the price magnitude is not the venue's.
    if (p.tick_source == "derived")
        warn("TICK_SIZE_DERIVED",
             "no instrument spec at T0: prices rounded to a tick derived from the price magnitude",
             "warning");
    if (in.ctx.data_quality == ContextQuality::degraded_3_of_4)
        warn("DEGRADED_3_OF_4", "3 of 4 venues contributed to this context", "warning");
    if (in.ctx.data_quality == ContextQuality::degraded_2_of_4)
        warn("DEGRADED_2_OF_4", "2 of 4 venues contributed to this context", "risk_flag");
    if (c.cross_venue_confirmation < kMinCrossVenueAgreement)
        warn("LOW_CROSS_VENUE_AGREEMENT",
             ("venue agreement " + num(c.cross_venue_confirmation) + " < threshold " +
              num(kMinCrossVenueAgreement)).c_str(), "warning");
    if (in.ctx.spread_bps > kWideSpreadBps)
        warn("WIDE_SPREAD",
             ("spread " + num(in.ctx.spread_bps) + " bps > threshold " + num(kWideSpreadBps) +
              " bps (" + (in.ctx.spread_observed ? "measured" : "UNOBSERVED, conservative stand-in") +
              ")").c_str(), "risk_flag");
    if (in.ctx.expected_slippage_bps > kElevatedSlippageBps)
        warn_over("ELEVATED_SLIPPAGE", "expected slippage", in.ctx.expected_slippage_bps,
                  kElevatedSlippageBps, " bps", "risk_flag");
    if (c.effective_sample_size < kMinEffectiveSamples)
        warn("LOW_STRATEGY_SAMPLE",
             ("effective sample size " + num(c.effective_sample_size, 1) + " < threshold " +
              num(kMinEffectiveSamples, 0)).c_str(), "warning");
    if (in.ctx.regime_divergence)
        warn("REGIME_DIVERGENCE",
             ("Bitget regime '" + in.ctx.bitget_regime.regime + "' differs from global '" +
              in.ctx.global_regime.regime + "'").c_str(), "warning");
    if (std::abs(in.ctx.funding_rate) > kHighFundingRate)
        warn("HIGH_FUNDING",
             ("funding " + num(in.ctx.funding_rate * 100.0, 4) + "% > threshold " +
              num(kHighFundingRate * 100.0, 4) + "% (8h)").c_str(), "warning");
    for (const auto& w : c.warnings) warn(w.c_str(), "", "warning");
    for (const auto& w : s.warnings)
        if (w.severity == "risk_flag") s.risk_flags.push_back(w.code);

    // Validity gate + grade.
    const auto bad_reasons = invalid_reasons(p, in.ctx, now_ms);
    const bool invalid = !bad_reasons.empty();
    s.proposal_grade = assign_grade(c, ml, in.ensemble, invalid);
    if (invalid) {
        for (const auto& br : bad_reasons) s.reason_codes.push_back(br);
        s.recommended_for_execution = false;
        s.state = ProposalState::draft;   // never PROPOSED as executable
        return s;
    }

    // A strategy cell's realised expectancy is a DIAGNOSTIC of the setup as
    // a trader, recorded; it vetoes nothing. A bad historical trader can be an
    // informative event detector, and deciding what to trade is VPS2's job.
    const bool edge_is_measured = c.effective_sample_size >= kMinEssMeasured;
    if (edge_is_measured && c.strategy_posterior < 0.0) {
        warn("NEGATIVE_MEASURED_EDGE",
             "diagnostic: this strategy/regime/side has a negative realised expectancy over "
             "a sufficient sample", "info");
    }

    // VPS1 takes no execution decision. The flag stays on the wire (the
    // contract carries it so a consumer never infers it from a signal's
    // presence) and is always false: whether to act on a signal -- and with
    // what order, size or risk -- is decided downstream, in VPS2.
    s.recommended_for_execution = false;
    return s;
}

} // namespace altstrategy::engine
