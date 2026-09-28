#include "altsignalgateway/domain/canonical.hpp"

#include "altsignalgateway/core/hash.hpp"
#include "altsignalgateway/domain/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace altsignalgateway::domain {

namespace pb = altsignalgateway::v1;
namespace as = altstrategy::v1;
namespace sq = altsignalquality::v1;

namespace {

// AltStrategy encodes an absent optional price as exactly 0.0. Anything
// non-finite is absent too: a NaN that reaches a consumer as a price is worse
// than a missing field, because it compares false against every bound.
bool price_present(double v) { return std::isfinite(v) && v > 0.0; }

void fill_named(google::protobuf::RepeatedPtrField<pb::NamedValue>* out,
                const std::vector<NamedScalar>& in) {
    // Sorted by name so the serialization is canonical regardless of how the
    // loader happened to iterate. See the NamedValue comment in gateway.proto.
    std::vector<const NamedScalar*> ordered;
    ordered.reserve(in.size());
    for (const auto& v : in) {
        if (!std::isfinite(v.value)) continue;  // absent, never a substitute
        ordered.push_back(&v);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const NamedScalar* a, const NamedScalar* b) { return a->name < b->name; });
    for (const auto* v : ordered) {
        auto* nv = out->Add();
        nv->set_name(v->name);
        nv->set_value(v->value);
    }
}

void fill_regime(pb::RegimeView* out, const RegimeSnapshot& in) {
    out->set_regime(in.regime);
    if (in.confidence && std::isfinite(*in.confidence)) {
        out->set_has_confidence(true);
        out->set_confidence(*in.confidence);
    }
    if (in.trend_strength && std::isfinite(*in.trend_strength)) {
        out->set_has_trend_strength(true);
        out->set_trend_strength(*in.trend_strength);
    }
    if (in.volatility_regime && std::isfinite(*in.volatility_regime)) {
        out->set_has_volatility_regime(true);
        out->set_volatility_regime(*in.volatility_regime);
    }
    if (in.liquidity_quality && std::isfinite(*in.liquidity_quality)) {
        out->set_has_liquidity_quality(true);
        out->set_liquidity_quality(*in.liquidity_quality);
    }
}

// A probability vector that is entirely zero is what an unpopulated proposal
// looks like — not a forecast that nothing can happen.
bool any_nonzero_finite(std::initializer_list<double> vs) {
    for (double v : vs) {
        if (std::isfinite(v) && v != 0.0) return true;
    }
    return false;
}

} // namespace

as::ContextQuality parse_context_quality(std::string_view token) {
    if (token == "STRICT_4_OF_4") return as::CONTEXT_STRICT_4_OF_4;
    if (token == "DEGRADED_3_OF_4") return as::CONTEXT_DEGRADED_3_OF_4;
    if (token == "DEGRADED_2_OF_4") return as::CONTEXT_DEGRADED_2_OF_4;
    if (token == "INVALID") return as::CONTEXT_INVALID;
    return as::CONTEXT_QUALITY_UNSPECIFIED;
}

void fill_macro(pb::MacroSnapshot* m, const MacroSnapshot& macro) {
    if (!macro.present) {
        m->set_present(false);
        m->set_absence_reason(macro.absence_reason.empty() ? "MACRO_UNAVAILABLE"
                                                           : macro.absence_reason);
        return;
    }
    m->set_present(true);
    m->set_feature_set_name(macro.feature_set_name);
    m->set_feature_set_version(macro.feature_set_version);
    m->set_schema_hash(macro.schema_hash);
    m->set_method_version(macro.method_version);
    m->set_bar_open_time_ms(macro.bar_open_time_ms);
    m->set_computed_at_ms(macro.computed_at_ms);
    m->set_age_ms(macro.age_ms);
    m->set_status(macro.status);
    m->set_feature_count(macro.feature_count);
    m->set_valid_count(macro.valid_count);
    fill_named(m->mutable_values(), macro.values);
    for (const auto& n : macro.unavailable) m->add_unavailable(n);
    m->set_source_service("alttech");
}

pb::SignalSummary summarize_payload(const pb::SignalPayload& payload,
                                    std::string_view delivery_state, int64_t last_ack_at_ms) {
    pb::SignalSummary s;
    const auto& t = payload.trade_plan();
    s.set_exchange(t.exchange());
    s.set_symbol(t.symbol());
    s.set_canonical_pair_id(t.canonical_pair_id());
    s.set_side(t.side());
    s.set_strategy_id(t.strategy_id());
    s.set_strategy_version(t.strategy_version());
    s.set_entry_type(t.entry_type());
    s.set_entry_price(t.entry_price());
    s.set_stop_loss_price(t.stop_loss_price());
    s.set_take_profit_price(t.take_profit_price());
    s.set_expected_rr(t.expected_rr());
    s.set_holding_class(t.holding_class());
    s.set_recommended_for_execution(payload.strategy().recommended_for_execution());
    s.set_strategy_confidence(payload.strategy().strategy_confidence());
    s.set_quality_decision(payload.signal_quality().decision());
    s.set_has_quality_score(payload.signal_quality().has_quality_score());
    s.set_quality_score(payload.signal_quality().quality_score());
    s.set_altml_present(payload.altml().present());
    s.set_altml_conviction(payload.altml().conviction());
    s.set_market_snapshot_present(payload.market_snapshot().present());
    s.set_macro_present(payload.macro().present());
    s.set_delivery_state(std::string(delivery_state));
    s.set_last_ack_at_ms(last_ack_at_ms);
    s.set_market_forecast_present(payload.market_forecast().present());
    s.set_evaluation_horizon_minutes(payload.market_forecast().evaluation_horizon_minutes());
    return s;
}

pb::SignalPayload build_payload(const sq::FinalTradeSignal& sig, std::string_view source_envelope,
                                const ContextSnapshot& ctx, const InstrumentResolution& inst,
                                const MacroSnapshot& macro, const IngestMetadata& meta) {
    const as::TradeSignalProposal& p = sig.proposal();
    const sq::SignalQualityEvaluation& q = sig.quality();

    pb::SignalPayload out;

    // ---------------------------------------------------------------- trade
    {
        pb::TradePlan* t = out.mutable_trade_plan();
        // Instrument identity. The executable symbol comes from AltSpotter's
        // registry, never from the NATS subject token: on this deployment the
        // upstream symbol field carries a fallback like "pair-16", which no
        // venue has ever heard of.
        t->set_exchange(inst.exchange);
        t->set_symbol(inst.exchange_symbol);
        t->set_canonical_pair_id(p.canonical_pair_id());
        t->set_base_asset(inst.base_asset);
        t->set_quote_asset(inst.quote_asset);
        t->set_market_type(inst.market_type);

        t->set_side(p.side());
        t->set_entry_type(p.entry_type());
        t->set_entry_price(p.entry_price());
        if (price_present(p.entry_lower()) && price_present(p.entry_upper())) {
            t->set_has_entry_range(true);
            t->set_entry_lower(p.entry_lower());
            t->set_entry_upper(p.entry_upper());
        }
        if (price_present(p.trigger_price())) {
            t->set_has_trigger_price(true);
            t->set_trigger_price(p.trigger_price());
        }
        t->set_max_slippage_bps(p.max_slippage_bps());
        t->set_post_only_allowed(p.post_only_allowed());
        t->set_expected_fill_time_p50_min(p.expected_fill_time_p50_min());
        t->set_expected_fill_time_p90_min(p.expected_fill_time_p90_min());
        t->set_entry_fill_timeout_ms(p.entry_fill_timeout_ms());

        t->set_stop_loss_price(p.stop_loss_price());
        t->set_stop_basis(p.stop_basis());
        t->set_stop_distance_bps(p.stop_distance_bps());
        t->set_stop_distance_atr(p.stop_distance_atr());
        t->set_take_profit_price(p.take_profit_price());
        if (price_present(p.optional_secondary_target())) {
            t->set_has_secondary_target(true);
            t->set_secondary_target_price(p.optional_secondary_target());
        }
        t->set_target_basis(p.target_basis());
        t->set_target_distance_bps(p.target_distance_bps());
        t->set_expected_rr(p.expected_rr());

        t->set_holding_class(p.holding_class());
        t->set_expected_holding_time_p50_ms(p.expected_holding_time_p50_ms());
        t->set_expected_holding_time_p90_ms(p.expected_holding_time_p90_ms());
        t->set_max_holding_time_ms(p.max_holding_time_ms());
        t->set_time_stop_at_ms(p.time_stop_at_ms());
        t->set_valid_until_ms(p.valid_until_ms());

        t->set_strategy_id(p.strategy_id());
        t->set_strategy_version(p.strategy_version());
    }

    // ------------------------------------------------------------- strategy
    {
        pb::StrategyBlock* s = out.mutable_strategy();
        s->set_strategy_id(p.strategy_id());
        s->set_strategy_version(p.strategy_version());
        s->set_parameter_set_id(p.parameter_set_id());
        s->set_candidate_id(p.candidate_id());
        s->set_ranking_id(p.ranking_id());
        s->set_ranking_policy_id(p.ranking_policy_id());
        s->set_proposal_grade(p.proposal_grade());
        s->set_state(p.state());
        s->set_plan_version(p.plan_version());
        s->set_recommended_for_execution(p.recommended_for_execution());
        s->set_strategy_confidence(p.strategy_confidence());
        s->set_ensemble_agreement(p.ensemble_agreement());
        s->set_adaptive_score(p.adaptive_score());
        s->set_context_quality(p.quality());

        // `edge_estimated` is the producer's own answer to "did a model produce
        // these numbers", so it is the gate -- not a nonzero test over the
        // values themselves. A forecast may legitimately estimate a net return
        // of exactly zero, and a value-based probe would then drop it; worse, it
        // would let a single real number drag zeros out alongside it.
        //
        // Four of the five members this used to probe (p_entry_reached,
        // p_stop_before_target, p_timeout, p_ambiguous) and two of the three
        // returns (expected_gross_return, expected_utility) no longer exist:
        // AltStrategy reserved their tags because nothing ever computed them.
        // The vendored contract here was ten days stale and still declared them,
        // so they read as protobuf zeros and were transmitted as measurements --
        // exactly the reading the producer removed them to prevent.
        if (p.edge_estimated()) {
            s->set_has_outcome_probabilities(true);
            s->set_p_target_before_stop(p.p_target_before_stop());
            s->set_has_expected_returns(true);
            s->set_expected_net_return(p.expected_net_return());
        }
        for (const auto& code : p.reason_codes()) s->add_reason_codes(code);
        for (const auto& flag : p.risk_flags()) s->add_risk_flags(flag);
        for (const auto& w : p.warnings()) *s->add_warnings() = w;
    }

    // ---------------------------------------------------------------- AltML
    {
        pb::AltMlBlock* m = out.mutable_altml();
        // model_versions is AltStrategy's own record of a forecast having been
        // attached to this plan. Without it, ml_abstain and ml_conviction are
        // protobuf defaults, and forwarding them would report a confident model
        // that never ran.
        //
        // What is forwarded is what AltStrategy actually produces. `uncertainty`
        // and `ood_score` used to be sent here and are gone: their tags are
        // reserved upstream ("`ml_ood` was never produced by anything"), so both
        // were structurally zero on every signal -- and `ood_score: 0` reads as
        // a measured all-clear on a check that never ran. `conviction` replaces
        // them, and is the one ML number this stack computes: |p_up - 0.5| * 2,
        // distance from a coin flip, which is not an uncertainty and must not be
        // labelled as one.
        if (p.model_versions_size() > 0) {
            m->set_present(true);
            for (const auto& v : p.model_versions()) m->add_model_versions(v);
            m->set_abstain(p.ml_abstain());
            m->set_conviction(p.ml_conviction());
        } else {
            m->set_present(false);
            m->set_absence_reason("NO_MODEL_VERSIONS_ON_PROPOSAL");
        }
    }

    // -------------------------------------------------------- signal quality
    {
        pb::SignalQualityBlock* g = out.mutable_signal_quality();
        g->set_evaluation_id(q.evaluation_id());
        g->set_decision(q.decision());
        g->set_basis(q.basis());
        g->set_has_quality_score(q.has_quality_score());
        if (q.has_quality_score()) {
            g->set_quality_score(q.quality_score());
            g->set_raw_score(q.raw_score());
            g->set_decision_threshold(q.decision_threshold());
        }
        for (const auto& code : q.reason_codes()) g->add_reason_codes(code);
        g->set_model_id(q.model_id());
        g->set_model_version(q.model_version());
        g->set_feature_schema_version(q.feature_schema_version());
        g->set_feature_schema_hash(q.feature_schema_hash());
        g->set_calibration_version(q.calibration_version());
        g->set_label_spec_version(q.label_spec_version());
        g->set_feature_count(q.feature_count());
        g->set_missing_feature_count(q.missing_feature_count());
        g->set_evaluated_at_ms(q.evaluated_at_ms());
        g->set_inference_latency_us(q.inference_latency_us());
    }

    // ------------------------------------------------------- market snapshot
    {
        pb::MarketSnapshot* ms = out.mutable_market_snapshot();
        if (!ctx.present) {
            ms->set_present(false);
            ms->set_absence_reason(ctx.absence_reason.empty() ? "CONTEXT_SNAPSHOT_UNAVAILABLE"
                                                              : ctx.absence_reason);
        } else {
            ms->set_present(true);
            ms->set_event_time_ms(ctx.event_time_ms);
            ms->set_available_at_ms(ctx.available_at_ms);
            ms->set_expires_at_ms(ctx.expires_at_ms);
            ms->set_context_revision(ctx.context_revision);
            // Each venue scalar is presence-gated on its own: AltStrategy
            // writes NULL for "not measured", and a 0 bps spread and an
            // unknown spread are not the same market.
            const auto set_scalar = [ms](void (pb::MarketSnapshot::*has)(bool),
                                         void (pb::MarketSnapshot::*val)(double),
                                         const std::optional<double>& v) {
                if (v && std::isfinite(*v)) {
                    (ms->*has)(true);
                    (ms->*val)(*v);
                }
            };
            set_scalar(&pb::MarketSnapshot::set_has_mark_price,
                       &pb::MarketSnapshot::set_mark_price, ctx.mark_price);
            set_scalar(&pb::MarketSnapshot::set_has_spread_bps,
                       &pb::MarketSnapshot::set_spread_bps, ctx.spread_bps);
            set_scalar(&pb::MarketSnapshot::set_has_funding_rate,
                       &pb::MarketSnapshot::set_funding_rate, ctx.funding_rate);
            set_scalar(&pb::MarketSnapshot::set_has_fees_bps,
                       &pb::MarketSnapshot::set_fees_bps, ctx.fees_bps);
            set_scalar(&pb::MarketSnapshot::set_has_top_depth_usdt,
                       &pb::MarketSnapshot::set_top_depth_usdt, ctx.top_depth_usdt);
            set_scalar(&pb::MarketSnapshot::set_has_liquidity_rank,
                       &pb::MarketSnapshot::set_liquidity_rank, ctx.liquidity_rank);
            set_scalar(&pb::MarketSnapshot::set_has_expected_slippage_bps,
                       &pb::MarketSnapshot::set_expected_slippage_bps, ctx.expected_slippage_bps);

            fill_named(ms->mutable_venue_features(), ctx.venue_features);
            fill_named(ms->mutable_cross_venue_features(), ctx.cross_venue_features);
            fill_named(ms->mutable_reference_features(), ctx.reference_features);

            ms->set_global_universe_status(ctx.global_universe_status);
            ms->set_data_quality(parse_context_quality(ctx.data_quality));
            ms->set_venue_mask(ctx.venue_mask);
            ms->set_missing_venue_mask(ctx.missing_venue_mask);
        }
    }

    // ------------------------------------------------------- microstructure
    {
        pb::MicrostructureSnapshot* mi = out.mutable_microstructure();
        if (!ctx.present) {
            mi->set_present(false);
            mi->set_absence_reason(ctx.absence_reason.empty() ? "CONTEXT_SNAPSHOT_UNAVAILABLE"
                                                              : ctx.absence_reason);
        } else if (ctx.microstructure.empty()) {
            mi->set_present(false);
            mi->set_absence_reason("NO_MICROSTRUCTURE_IN_CONTEXT");
        } else {
            mi->set_present(true);
            mi->set_event_time_ms(ctx.event_time_ms);
            fill_named(mi->mutable_values(), ctx.microstructure);
        }
    }

    // ---------------------------------------------------------------- regime
    {
        pb::RegimeBlock* r = out.mutable_regime();
        if (!ctx.present) {
            r->set_present(false);
            r->set_absence_reason(ctx.absence_reason.empty() ? "CONTEXT_SNAPSHOT_UNAVAILABLE"
                                                             : ctx.absence_reason);
        } else if (ctx.venue_regime.empty() && ctx.global_regime.empty()) {
            r->set_present(false);
            r->set_absence_reason("NO_REGIME_IN_CONTEXT");
        } else {
            r->set_present(true);
            fill_regime(r->mutable_venue_regime(), ctx.venue_regime);
            fill_regime(r->mutable_global_regime(), ctx.global_regime);
            r->set_regime_divergence(ctx.regime_divergence);
        }
    }

    // ------------------------------------------------------ execution context
    {
        pb::ExecutionContext* e = out.mutable_execution_context();
        if (!inst.present) {
            e->set_present(false);
            e->set_absence_reason(inst.absence_reason.empty() ? "INSTRUMENT_UNRESOLVED"
                                                              : inst.absence_reason);
        } else {
            // The instrument resolved; the SPEC may still be missing, and those
            // are different failures with different consequences: without the
            // instrument there is no symbol at all, without the spec there is a
            // symbol whose tick size is unknown.
            e->set_present(inst.spec_present);
            if (!inst.spec_present) {
                e->set_absence_reason(inst.spec_absence_reason.empty() ? "INSTRUMENT_SPEC_MISSING"
                                                                       : inst.spec_absence_reason);
            }
            e->set_exchange(inst.exchange);
            e->set_exchange_instrument_id(inst.exchange_instrument_id);
            e->set_exchange_symbol(inst.exchange_symbol);
            e->set_instrument_status(inst.instrument_status);
            e->set_whitelisted(inst.whitelisted);
            e->set_resolved_at_ms(inst.resolved_at_ms);
            if (inst.spec_present) {
                e->set_spec_version(inst.spec_version);
                e->set_spec_valid_from_ms(inst.spec_valid_from_ms);
                e->set_contract_type(inst.contract_type);
                e->set_settle_asset(inst.settle_asset);
                e->set_tick_size(inst.tick_size);
                e->set_quantity_step(inst.quantity_step);
                e->set_min_order_quantity(inst.min_order_quantity);
                e->set_min_notional(inst.min_notional);
                e->set_contract_value(inst.contract_value);
                if (inst.price_precision) {
                    e->set_has_price_precision(true);
                    e->set_price_precision(*inst.price_precision);
                }
                if (inst.quantity_precision) {
                    e->set_has_quantity_precision(true);
                    e->set_quantity_precision(*inst.quantity_precision);
                }
                if (inst.max_leverage && std::isfinite(*inst.max_leverage)) {
                    e->set_has_max_leverage(true);
                    e->set_max_leverage(*inst.max_leverage);
                }
            }
        }
    }

    // ------------------------------------------------------------ provenance
    {
        pb::Provenance* pr = out.mutable_provenance();
        pr->set_signal_proposal_id(p.signal_proposal_id());
        pr->set_candidate_id(p.candidate_id());
        pr->set_ranking_id(p.ranking_id());
        pr->set_final_signal_id(sig.final_signal_id());
        pr->set_evaluation_id(q.evaluation_id());
        pr->set_context_id(q.context_id());
        pr->set_context_revision(p.context_revision());

        pr->set_feature_manifest_hash(p.feature_manifest_hash());
        pr->set_quality_feature_schema_version(q.feature_schema_version());
        pr->set_quality_feature_schema_hash(q.feature_schema_hash());
        pr->set_quality_model_id(q.model_id());
        pr->set_quality_model_version(q.model_version());
        pr->set_quality_calibration_version(q.calibration_version());
        pr->set_quality_label_spec_version(q.label_spec_version());
        for (const auto& v : p.model_versions()) pr->add_altml_model_versions(v);
        pr->set_strategy_id(p.strategy_id());
        pr->set_strategy_version(p.strategy_version());
        pr->set_parameter_set_id(p.parameter_set_id());
        pr->set_ranking_policy_id(p.ranking_policy_id());

        pr->set_signal_created_at_ms(p.created_at_ms());
        if (ctx.present) {
            pr->set_context_event_time_ms(ctx.event_time_ms);
            pr->set_context_available_at_ms(ctx.available_at_ms);
        }
        pr->set_quality_evaluated_at_ms(q.evaluated_at_ms());
        pr->set_quality_published_at_ms(sig.published_at_ms());
        pr->set_gateway_received_at_ms(meta.received_at_ms);
        pr->set_gateway_journaled_at_ms(meta.journaled_at_ms);

        pr->set_altsignalquality_version(sig.producer_version());
        pr->set_altsignalgateway_version(meta.gateway_version);

        pr->set_source_envelope_sha256(sha256_hex(source_envelope));
        pr->set_source_subject(meta.source_subject);
        pr->set_source_stream(meta.source_stream);
        pr->set_source_stream_sequence(meta.source_stream_sequence);
    }

    // ----------------------------------------------------------------- macro
    fill_macro(out.mutable_macro(), macro);

    // ------------------------------------------------- market forecast (s3)
    {
        pb::MarketForecastBlock* f = out.mutable_market_forecast();
        if (p.market_predictions_size() == 0) {
            f->set_present(false);
            f->set_absence_reason("NO_MARKET_PREDICTIONS_ON_PROPOSAL");
        } else {
            f->set_present(true);
            f->set_evaluation_horizon_minutes(p.evaluation_horizon_minutes());
            f->set_signal_semantics(p.signal_semantics());
            // Ascending horizon, whatever order the producer sent: the payload
            // is hashed, so its bytes must not depend on it.
            std::vector<const as::MarketPrediction*> hs;
            for (const auto& h : p.market_predictions()) hs.push_back(&h);
            std::stable_sort(hs.begin(), hs.end(), [](const auto* a, const auto* b) {
                return a->horizon_minutes() < b->horizon_minutes();
            });
            for (const auto* h : hs) {
                pb::HorizonForecast* o = f->add_horizons();
                o->set_horizon_minutes(h->horizon_minutes());
                o->set_state(h->state());
                o->set_model_id(h->model_id());
                o->set_model_version(h->model_version());
                o->set_model_role(h->model_role());
                o->set_calibration_version(h->calibration_version());
                o->set_probability_calibrated(h->probability_calibrated());
                o->set_bar_open_time_ms(h->bar_open_time_ms());
                o->set_computed_at_ms(h->computed_at_ms());
                o->set_aligned_missing(h->aligned_missing());
                if (h->bar_open_time_ms() > 0) {
                    o->set_age_ms(p.created_at_ms() - (h->bar_open_time_ms() + 60'000));
                }
                // The absence rule: numbers travel only under the producer's
                // own has_values, and only when they are what they claim to be
                // -- finite, probabilities in [0,1] summing to one, excursions
                // not negative. A value that is not is refused here as absent
                // rather than passed on as a measurement.
                const bool finite = std::isfinite(h->p_up()) && std::isfinite(h->p_flat()) &&
                                    std::isfinite(h->p_down()) && std::isfinite(h->expected_return_bps()) &&
                                    std::isfinite(h->expected_up_excursion_bps()) &&
                                    std::isfinite(h->expected_down_excursion_bps());
                const bool coherent =
                    finite && h->p_up() >= 0 && h->p_flat() >= 0 && h->p_down() >= 0 &&
                    std::abs(h->p_up() + h->p_flat() + h->p_down() - 1.0) < 1e-3 &&
                    h->expected_up_excursion_bps() >= 0 && h->expected_down_excursion_bps() >= 0;
                if (h->has_values() && coherent) {
                    o->set_present(true);
                    o->set_p_up(h->p_up());
                    o->set_p_flat(h->p_flat());
                    o->set_p_down(h->p_down());
                    o->set_expected_return_bps(h->expected_return_bps());
                    o->set_expected_up_excursion_bps(h->expected_up_excursion_bps());
                    o->set_expected_down_excursion_bps(h->expected_down_excursion_bps());
                    o->set_cost_model_version(h->cost_model_version());
                    if (std::isfinite(h->cost_bps())) o->set_cost_bps(h->cost_bps());
                    if (std::isfinite(h->expected_net_long_bps())) o->set_expected_net_long_bps(h->expected_net_long_bps());
                    if (std::isfinite(h->expected_net_short_bps())) o->set_expected_net_short_bps(h->expected_net_short_bps());
                } else {
                    o->set_present(false);
                    if (h->has_values()) o->set_state("INCOHERENT_VALUES");
                }
            }
        }
    }

    // ------------------------------------------------------------ setup (s3)
    {
        pb::SetupBlock* b = out.mutable_setup();
        b->set_setup_id(p.strategy_id());
        std::vector<std::pair<std::string, double>> d;
        for (const auto& x : p.setup_descriptors()) {
            if (std::isfinite(x.value())) d.emplace_back(x.name(), x.value());
        }
        std::sort(d.begin(), d.end());
        for (const auto& [name, value] : d) {
            auto* nv = b->add_descriptors();
            nv->set_name(name);
            nv->set_value(value);
        }
    }

    out.set_source_envelope(std::string(source_envelope));
    return out;
}

pb::SignalEnvelope build_envelope(const EnvelopeSource& src, bool replay,
                                  uint32_t delivery_attempt, int64_t published_at_ms) {
    pb::SignalEnvelope env;
    env.set_schema_version(src.schema_version == 0 ? kSchemaVersion : src.schema_version);
    env.set_protocol_version(kProtocolVersion);
    env.set_message_id(src.message_id);
    env.set_signal_id(src.signal_id);
    env.set_sequence(src.sequence);
    env.set_signal_type("FINAL_TRADE_SIGNAL");
    env.set_created_at_ms(src.created_at_ms);
    env.set_expires_at_ms(src.expires_at_ms);
    env.set_journaled_at_ms(src.journaled_at_ms);
    env.set_published_at_ms(published_at_ms);
    env.set_payload_sha256(src.payload_sha256);
    env.set_producer_service("altsignalgateway");
    env.set_producer_version(src.producer_version);
    env.set_replay(replay);
    env.set_delivery_attempt(delivery_attempt);
    return env;
}

} // namespace altsignalgateway::domain
