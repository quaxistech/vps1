#include "altsignalquality/components/evaluator.hpp"

#include "altsignalquality/components/model_runtime.hpp"
#include "altsignalquality/components/publication_evidence.hpp"
#include "altsignalquality/core/codec.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/nats_subjects.hpp"
#include "altsignalquality/core/pg.hpp"
#include "altsignalquality/core/readiness.hpp"
#include "altsignalquality/core/shutdown.hpp"
#include "altsignalquality/core/time.hpp"
#include "altsignalquality/data/store.hpp"
#include "altsignalquality/domain/feature_contract.hpp"
#include "altsignalquality/domain/labeling.hpp"
#include "altsignalquality/domain/policy.hpp"
#include "altsignalquality/domain/priors.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace altsignalquality {

namespace {

namespace sq = altsignalquality::v1;
namespace as = altstrategy::v1;

// Flatten the wire proposal into the pure-domain candidate. Every field is
// copied straight across; nothing is recomputed, so what the model sees is what
// AltStrategy published.
domain::CandidateSignal to_candidate(const as::TradeSignalProposal& p,
                                     const std::string& symbol) {
    domain::CandidateSignal c;
    c.signal_proposal_id = p.signal_proposal_id();
    c.candidate_id = p.candidate_id();
    c.ranking_id = p.ranking_id();
    c.canonical_pair_id = p.canonical_pair_id();
    c.bitget_instrument_id = p.bitget_instrument_id();
    c.symbol = symbol;
    c.strategy_id = p.strategy_id();
    c.strategy_version = p.strategy_version();
    c.parameter_set_id = p.parameter_set_id();
    c.side = static_cast<int>(p.side());
    c.proposal_grade = as::ProposalGrade_Name(p.proposal_grade());
    // The generated enum names carry their proto prefixes ("GRADE_STRONG",
    // "CONTEXT_STRICT_4_OF_4"); the vocabulary and the database CHECKs use the
    // bare tokens AltStrategy itself persists.
    if (c.proposal_grade.rfind("GRADE_", 0) == 0) c.proposal_grade.erase(0, 6);
    c.plan_version = p.plan_version();
    c.holding_class = static_cast<int>(p.holding_class());
    c.quality = as::ContextQuality_Name(p.quality());
    if (c.quality.rfind("CONTEXT_", 0) == 0) c.quality.erase(0, 8);
    c.created_at_ms = p.created_at_ms();
    c.valid_until_ms = p.valid_until_ms();
    c.entry_type = static_cast<int>(p.entry_type());
    c.entry_price = p.entry_price();
    c.trigger_price = p.trigger_price();
    c.max_slippage_bps = p.max_slippage_bps();
    c.post_only_allowed = p.post_only_allowed();
    c.expected_fill_time_p50_min = p.expected_fill_time_p50_min();
    c.expected_fill_time_p90_min = p.expected_fill_time_p90_min();
    c.entry_fill_timeout_ms = p.entry_fill_timeout_ms();
    c.stop_loss_price = p.stop_loss_price();
    c.stop_distance_bps = p.stop_distance_bps();
    c.stop_distance_atr = p.stop_distance_atr();
    c.take_profit_price = p.take_profit_price();
    c.target_distance_bps = p.target_distance_bps();
    c.expected_holding_time_p50_ms = p.expected_holding_time_p50_ms();
    c.expected_holding_time_p90_ms = p.expected_holding_time_p90_ms();
    c.max_holding_time_ms = p.max_holding_time_ms();
    c.time_stop_at_ms = p.time_stop_at_ms();
    c.edge_estimated = p.edge_estimated();
    if (p.has_p_target_before_stop()) c.p_target_before_stop = p.p_target_before_stop();
    c.expected_net_return = p.expected_net_return();
    c.expected_rr = p.expected_rr();
    c.ml_conviction = p.ml_conviction();
    c.ml_abstain = p.ml_abstain();
    c.strategy_confidence = p.strategy_confidence();
    c.ensemble_agreement = p.ensemble_agreement();
    c.adaptive_score = p.adaptive_score();
    c.risk_flag_count = p.risk_flags_size();
    c.warning_count = p.warnings_size();
    c.reason_code_count = p.reason_codes_size();
    c.alternative_plan_count = p.alternative_plan_ids_size();
    c.recommended_for_execution = p.recommended_for_execution();
    c.context_revision = p.context_revision();
    c.feature_manifest_hash = p.feature_manifest_hash();
    return c;
}

// A candidate that fails these is not a signal this service can reason about.
// Rejecting it is a decision, so it is recorded like any other.
std::string validate_candidate(const domain::CandidateSignal& c) {
    if (c.signal_proposal_id.empty()) return "missing signal_proposal_id";
    if (c.candidate_id.empty()) return "missing candidate_id";
    if (c.canonical_pair_id <= 0) return "missing canonical_pair_id";
    if (c.strategy_id.empty()) return "missing strategy_id";
    if (c.side != 1 && c.side != 2) return "side is neither LONG nor SHORT";
    if (c.created_at_ms <= 0) return "missing created_at_ms";
    if (c.proposal_grade == "INVALID") return "INVALID proposals are never executable";
    if (!(c.stop_loss_price > 0) || !(c.take_profit_price > 0)) return "missing plan geometry";
    return {};
}

sq::DecisionBasis basis_to_wire(domain::DecisionBasis b) {
    switch (b) {
        case domain::DecisionBasis::model: return sq::BASIS_MODEL;
        case domain::DecisionBasis::cold_start: return sq::BASIS_COLD_START;
        case domain::DecisionBasis::model_unusable: return sq::BASIS_MODEL_UNUSABLE;
        case domain::DecisionBasis::upstream_not_recommended:
            return sq::BASIS_UPSTREAM_NOT_RECOMMENDED;
        case domain::DecisionBasis::malformed_signal: return sq::BASIS_MALFORMED_SIGNAL;
        case domain::DecisionBasis::score_below_floor: return sq::BASIS_SCORE_BELOW_FLOOR;
        case domain::DecisionBasis::signal_stale: return sq::BASIS_SIGNAL_STALE;
        case domain::DecisionBasis::holding_exceeds_ceiling:
            return sq::BASIS_HOLDING_EXCEEDS_CEILING;
    }
    return sq::DECISION_BASIS_UNSPECIFIED;
}

struct EvaluatorConfig {
    int64_t postgres_pool_size = 3;
    int64_t fetch_batch = 32;
    int64_t fetch_timeout_ms = 2000;
    int64_t model_poll_sec = 30;
    int64_t prior_reload_sec = 300;
    int64_t max_signal_age_sec = 900;
    bool publish_evaluations = true;
    domain::DecisionConfig decision;
};

Result<EvaluatorConfig> load_evaluator_config(const YAML::Node& root) {
    EvaluatorConfig c;
    const YAML::Node n = root["evaluator"];
    c.postgres_pool_size = config::opt_int(n, "postgres_pool_size", c.postgres_pool_size);
    c.fetch_batch = config::opt_int(n, "fetch_batch", c.fetch_batch);
    c.fetch_timeout_ms = config::opt_int(n, "fetch_timeout_ms", c.fetch_timeout_ms);
    c.model_poll_sec = config::opt_int(n, "model_poll_interval_sec", c.model_poll_sec);
    c.prior_reload_sec = config::opt_int(n, "prior_reload_interval_sec", c.prior_reload_sec);
    c.max_signal_age_sec = config::opt_int(n, "max_signal_age_sec", c.max_signal_age_sec);
    c.publish_evaluations = config::opt_bool(n, "publish_evaluations", c.publish_evaluations);

    const YAML::Node p = root["policy"];
    const std::string cold = config::opt_str(p, "cold_start", "PASS_THROUGH_ALL");
    const auto parsed = domain::parse_cold_start_policy(cold);
    if (!parsed) {
        return fail(ErrorCode::config_error,
                    cold == "PASS_THROUGH_RECOMMENDED"
                        ? std::string("policy.cold_start PASS_THROUGH_RECOMMENDED is retired: it made "
                                      "AltStrategy's economic verdict the gate, and VPS1 takes no "
                                      "execution decision (use PASS_THROUGH_ALL)")
                        : "policy.cold_start must be PASS_THROUGH_ALL or REJECT_ALL (got '" + cold + "')");
    }
    c.decision.cold_start = *parsed;
    c.decision.require_upstream_recommendation =
        config::opt_bool(p, "require_upstream_recommendation",
                         c.decision.require_upstream_recommendation);
    if (c.decision.require_upstream_recommendation) {
        return fail(ErrorCode::config_error,
                    "policy.require_upstream_recommendation is retired: an execution recommendation "
                    "is not VPS1's to make, and this service decides validity only (set it to false "
                    "or remove it)");
    }
    c.decision.min_quality_score = config::opt_double(p, "min_quality_score",
                                                      c.decision.min_quality_score);
    if (c.decision.min_quality_score != 0.0) {
        return fail(ErrorCode::config_error,
                    "policy.min_quality_score is retired: this service decides validity, and a "
                    "learned score never gates a signal (set it to 0 or remove it)");
    }
    if (c.fetch_batch < 1 || c.fetch_batch > 1024 || c.fetch_timeout_ms < 100 ||
        c.postgres_pool_size < 1 || c.model_poll_sec < 1 || c.prior_reload_sec < 1 ||
        c.max_signal_age_sec < 1) {
        return fail(ErrorCode::config_error, "invalid evaluator configuration bounds");
    }
    return c;
}

} // namespace

struct EvaluatorComponent::Impl {
    EvaluatorConfig cfg;
    std::optional<persistence::PgPool> pool;
    std::optional<data::Store> store;
    std::optional<ModelHolder> models;
    std::optional<messaging::PullConsumer> consumer;
    domain::PriorAccumulator priors;
    std::mutex priors_mu;
    int64_t next_model_poll_ms = 0;
    int64_t next_prior_reload_ms = 0;
    int64_t evaluated = 0, accepted = 0, duplicates = 0, malformed = 0;
};

EvaluatorComponent::EvaluatorComponent(const config::RuntimeConfig& rt,
                                       messaging::NatsClient& nats)
    : rt_(rt), nats_(nats), impl_(std::make_unique<Impl>()) {}

EvaluatorComponent::~EvaluatorComponent() = default;

Result<void> EvaluatorComponent::init() {
    auto cfg = load_evaluator_config(rt_.root);
    if (!cfg) return std::unexpected(cfg.error());
    impl_->cfg = *cfg;

    impl_->pool.emplace(rt_.common.postgres_dsn, static_cast<size_t>(cfg->postgres_pool_size));
    if (auto probe = impl_->pool->with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); });
        !probe) {
        return fail(ErrorCode::database_error, "postgres unavailable: " + probe.error().message);
    }
    impl_->store.emplace(*impl_->pool, rt_.wiring.altstrategy_schema,
                         rt_.wiring.max_stop_distance_bps);
    if (auto r = impl_->store->register_contracts(); !r) return r;
    if (auto r = impl_->store->verify_upstream_readable(); !r) return r;

    impl_->models.emplace(*impl_->store, rt_.wiring.objective,
                          std::filesystem::path(rt_.wiring.model_root));
    impl_->models->refresh();

    if (auto r = impl_->store->load_prior_cells(impl_->priors); !r) {
        return fail(r.error().code, "could not load outcome priors: " + r.error().message);
    }

    auto consumer = nats_.pull_consumer(rt_.wiring.candidate_stream, rt_.wiring.candidate_durable);
    if (!consumer) return std::unexpected(consumer.error());
    impl_->consumer.emplace(std::move(*consumer));

    const auto model = impl_->models->current();
    log::info("evaluator_ready",
              {{"feature_schema", std::string(domain::kFeatureSchemaVersion)},
               {"feature_schema_hash", domain::feature_schema_hash()},
               {"feature_count", static_cast<int64_t>(domain::kFeatureCount)},
               {"label_spec", std::string(domain::kLabelSpecVersion)},
               {"model_id", model ? model->model_id() : ""},
               {"cold_start_policy",
                std::string(domain::cold_start_policy_name(impl_->cfg.decision.cold_start))},
               {"require_upstream_recommendation",
                impl_->cfg.decision.require_upstream_recommendation}});
    set_component_status(name(), model ? "serving" : "cold-start");
    return {};
}

namespace {

// One place that turns an evaluation into its wire form, so the row persisted
// and the message published can never describe different decisions.
sq::SignalQualityEvaluation to_wire_evaluation(const data::EvaluationRow& row,
                                               const as::TradeSignalProposal& proposal,
                                               domain::DecisionBasis basis, int32_t model_version) {
    sq::SignalQualityEvaluation e;
    e.set_evaluation_id(row.evaluation_id);
    e.set_signal_proposal_id(row.signal_proposal_id);
    e.set_candidate_id(row.candidate_id);
    e.set_canonical_pair_id(row.canonical_pair_id);
    e.set_symbol(row.symbol);
    e.set_strategy_id(row.strategy_id);
    e.set_strategy_version(row.strategy_version);
    // Side and grade are copied from the proposal's own enums rather than
    // re-derived from the persisted token: a round trip through the bare name
    // is one rename away from silently landing on UNSPECIFIED.
    e.set_side(proposal.side());
    e.set_proposal_grade(proposal.proposal_grade());
    e.set_upstream_recommended(row.upstream_recommended);
    e.set_has_quality_score(row.has_quality_score);
    e.set_quality_score(row.quality_score);
    e.set_raw_score(row.raw_score);
    e.set_decision_threshold(row.decision_threshold);
    e.set_decision(row.decision == "ACCEPTED" ? sq::DECISION_ACCEPTED : sq::DECISION_REJECTED);
    e.set_basis(basis_to_wire(basis));
    for (const auto& code : row.reason_codes) e.add_reason_codes(code);
    e.set_model_id(row.model_id);
    e.set_model_version(model_version);
    e.set_feature_schema_version(row.feature_schema_version);
    e.set_feature_schema_hash(row.feature_schema_hash);
    e.set_calibration_version(row.calibration_version);
    e.set_label_spec_version(std::string(domain::kLabelSpecVersion));
    e.set_feature_count(static_cast<int32_t>(domain::kFeatureCount));
    e.set_missing_feature_count(row.features.missing_count);
    e.set_context_id(row.context_id);
    e.set_context_revision(row.context_revision);
    e.set_signal_created_at_ms(row.signal_created_at_ms);
    e.set_evaluated_at_ms(row.evaluated_at_ms);
    e.set_inference_latency_us(row.inference_latency_us);
    return e;
}

} // namespace

void EvaluatorComponent::run() {
    auto& shutdown = ShutdownToken::instance();
    auto& cfg = impl_->cfg;
    auto& store = *impl_->store;
    const auto& wiring = rt_.wiring;

    // --- publish helpers -----------------------------------------------------
    // JetStream de-duplicates on Nats-Msg-Id, and both ids are deterministic in
    // the signal id, so a re-publish after a crash is a no-op rather than a
    // second final signal.
    const auto build_final = [&](const as::TradeSignalProposal& proposal,
                                   const sq::SignalQualityEvaluation& eval,
                                   const std::string& final_signal_id,
                                   const std::string& trace_id) -> std::string {
        messaging::EnvelopeMeta meta;
        meta.message_id = final_signal_id;
        meta.event_type = "final_trade_signal";
        meta.canonical_pair_id = eval.canonical_pair_id();
        meta.symbol = eval.symbol();
        meta.trace_id = trace_id;
        meta.correlation_id = eval.signal_proposal_id();
        auto env = messaging::make_envelope(meta);
        auto* fin = env.mutable_final_trade_signal();
        fin->set_final_signal_id(final_signal_id);
        *fin->mutable_proposal() = proposal;
        *fin->mutable_quality() = eval;
        fin->set_published_at_ms(timeutil::now_ms());
        fin->set_producer_version(ALTSIGNALQUALITY_VERSION);
        return messaging::serialize(env);
    };

    const auto publish_stored = [&](const data::ExistingEvaluation& row) -> Result<void> {
        auto evidence = restore_publication(row);
        if (!evidence) return std::unexpected(evidence.error());
        const auto& e = *evidence;
        const auto token = nats_subjects::subject_token(e.symbol, e.proposal.canonical_pair_id());
        if (auto r = nats_.js_publish(nats_subjects::final_signal(wiring, token),
                                      row.publication_payload, row.final_signal_id); !r)
            return r;
        return store.mark_published(e.proposal.signal_proposal_id(), timeutil::now_ms());
    };

    const auto publish_evaluation = [&](const sq::SignalQualityEvaluation& eval,
                                        const std::string& symbol_token,
                                        const std::string& trace_id) {
        if (!cfg.publish_evaluations) return;
        messaging::EnvelopeMeta meta;
        meta.message_id = eval.evaluation_id();
        meta.event_type = "signal_quality_evaluation";
        meta.canonical_pair_id = eval.canonical_pair_id();
        meta.symbol = eval.symbol();
        meta.trace_id = trace_id;
        meta.correlation_id = eval.signal_proposal_id();
        auto env = messaging::make_envelope(meta);
        *env.mutable_quality_evaluation() = eval;
        if (auto r = nats_.js_publish(nats_subjects::evaluation(wiring, symbol_token),
                                      messaging::serialize(env), eval.evaluation_id());
            !r) {
            // The audit stream is observability, not the decision. Losing one
            // message must never hold up a trade or force a redelivery.
            log::warn("evaluation_publish_failed",
                      {{"signal_proposal_id", eval.signal_proposal_id()},
                       {"message", r.error().message}});
        }
    };

    // --- restart recovery ----------------------------------------------------
    // An accepted evaluation with published = false is the crash window between
    // claiming the decision and putting it on the bus. Replay it before taking
    // new work, so a restart cannot swallow a signal.
    if (auto pending = store.load_unpublished(500); pending) {
        for (const auto& row : *pending) {
            if (auto r = publish_stored(data::ExistingEvaluation(row)); !r)
                log::error("republish_failed", {{"signal_proposal_id", row.signal_proposal_id},
                                                {"message", r.error().message}});
        }
    } else {
        log::warn("unpublished_scan_failed", {{"message", pending.error().message}});
    }

    // --- main loop -----------------------------------------------------------
    while (!shutdown.stop_requested()) {
        const int64_t now = timeutil::now_ms();
        if (now >= impl_->next_model_poll_ms) {
            impl_->models->refresh();
            impl_->next_model_poll_ms = now + cfg.model_poll_sec * 1000;
        }
        if (now >= impl_->next_prior_reload_ms) {
            domain::PriorAccumulator fresh;
            if (auto r = store.load_prior_cells(fresh); r) {
                std::lock_guard lock(impl_->priors_mu);
                impl_->priors = std::move(fresh);
            } else {
                log::warn("prior_reload_failed", {{"message", r.error().message}});
            }
            impl_->next_prior_reload_ms = now + cfg.prior_reload_sec * 1000;
        }

        auto batch = impl_->consumer->fetch(static_cast<int>(cfg.fetch_batch),
                                            cfg.fetch_timeout_ms);
        if (!batch) {
            // A fetch failure is a transport problem: nats.c reconnects on its
            // own, so back off briefly rather than tearing the component down.
            log::warn("candidate_fetch_failed", {{"message", batch.error().message}});
            if (shutdown.wait_for(std::chrono::seconds(2))) break;
            continue;
        }
        for (auto& msg : *batch) {
            if (shutdown.stop_requested()) break;
            std::string symbol, trace_id;
            auto parsed = messaging::parse_candidate(msg.data(), &symbol, &trace_id);
            if (!parsed) {
                // Unparseable or not a proposal: redelivering it forever would
                // wedge the consumer, so terminate it and record why.
                ++impl_->malformed;
                log::warn("candidate_message_discarded",
                          {{"subject", std::string(msg.subject())},
                           {"reason", parsed.error().message}});
                (void)msg.term();
                continue;
            }
            const auto candidate = to_candidate(*parsed, symbol);
            const std::string token =
                nats_subjects::subject_token(symbol, candidate.canonical_pair_id);

            if (const std::string bad = validate_candidate(candidate); !bad.empty()) {
                ++impl_->malformed;
                log::warn("candidate_rejected_malformed",
                          {{"signal_proposal_id", candidate.signal_proposal_id},
                           {"reason", bad}});
                (void)msg.term();
                continue;
            }
            // A candidate older than the configured window describes a market
            // that no longer exists. It is recorded and rejected, never scored:
            // a stale signal that scores well is still a stale signal.
            const int64_t age_ms = timeutil::now_ms() - candidate.created_at_ms;
            const bool stale = age_ms > cfg.max_signal_age_sec * 1000;

            bool input_conflict = false;
            auto processed = [&]() -> Result<void> {
                const auto replay_existing = [&](const data::ExistingEvaluation& prior) -> Result<void> {
                    if (!redelivery_matches(prior, msg.data())) {
                        input_conflict = true;
                        return fail(ErrorCode::validation_error, "same-ID candidate conflicts or lacks original integrity evidence");
                    }
                    ++impl_->duplicates;
                    if (prior.decision == "ACCEPTED" && !prior.published) return publish_stored(prior);
                    return {};
                };
                // Look up the immutable claim before consulting mutable model,
                // context, priors, age policy or feature state.
                auto prior = store.find_evaluation(candidate.signal_proposal_id);
                if (!prior) return std::unexpected(prior.error());
                if (*prior) return replay_existing(**prior);
                auto ctx = store.load_context_for_candidate(candidate.candidate_id);
                if (!ctx) return std::unexpected(ctx.error());

                domain::SignalSnapshot snapshot;
                snapshot.candidate = candidate;
                snapshot.candidate.context_id = ctx->context_id;
                snapshot.context = ctx->snapshot;
                {
                    std::lock_guard lock(impl_->priors_mu);
                    snapshot.priors = impl_->priors.snapshot(
                        candidate.created_at_ms, candidate.strategy_id,
                        candidate.canonical_pair_id, ctx->snapshot.global_regime,
                        candidate.proposal_grade, candidate.side);
                }
                const auto features = domain::assemble(snapshot);

                const auto model = impl_->models->current();
                ScoredSignal scored;
                bool have_score = false;
                bool inference_failed = false;
                double threshold = 0;
                if (model && !stale) {
                    auto s = model->score(features);
                    if (s) {
                        scored = *s;
                        have_score = true;
                        threshold = model->meta().decision_threshold;
                    } else {
                        inference_failed = true;
                        log::error("inference_failed",
                                   {{"signal_proposal_id", candidate.signal_proposal_id},
                                    {"model_id", model->model_id()},
                                    {"message", s.error().message}});
                    }
                }
                // "Unusable" means the registry named a model this process could
                // not use, or one that failed on this vector. Not scoring a
                // STALE signal is a deliberate policy decision, and reporting it
                // as a broken model would send an operator after the wrong
                // thing entirely.
                const bool model_unusable = impl_->models->unusable() || inference_failed;
                auto decision = domain::decide(cfg.decision, candidate.recommended_for_execution,
                                               have_score, scored.quality_score, threshold,
                                               model_unusable, stale,
                                               candidate.max_holding_time_ms);
                // A gap the proposal did not declare; the declared ones are in
                // missing_count, not in the reason.
                if (domain::undeclared_missing_count(features, snapshot.candidate) > 0) {
                    decision.reason_codes.emplace_back("MISSING_FEATURES");
                }
                if (!ctx->snapshot.present) {
                    decision.reason_codes.emplace_back("CONTEXT_SNAPSHOT_MISSING");
                }

                data::EvaluationRow row;
                row.signal_proposal_id = candidate.signal_proposal_id;
                row.evaluation_id = data::make_evaluation_id(candidate.signal_proposal_id);
                row.candidate_id = candidate.candidate_id;
                row.canonical_pair_id = candidate.canonical_pair_id;
                row.symbol = symbol.empty() ? token : symbol;
                row.strategy_id = candidate.strategy_id;
                row.strategy_version = candidate.strategy_version;
                row.parameter_set_id = candidate.parameter_set_id;
                row.side = std::string(domain::side_name(candidate.side));
                row.proposal_grade = candidate.proposal_grade;
                row.upstream_recommended = candidate.recommended_for_execution;
                row.regime = ctx->snapshot.global_regime;
                row.context_id = ctx->context_id;
                row.context_revision = candidate.context_revision;
                row.signal_created_at_ms = candidate.created_at_ms;
                row.evaluated_at_ms = timeutil::now_ms();
                row.feature_schema_version = std::string(domain::kFeatureSchemaVersion);
                row.feature_schema_hash = domain::feature_schema_hash();
                row.features = features;
                row.has_quality_score = have_score;
                row.raw_score = scored.raw_score;
                row.quality_score = scored.quality_score;
                row.decision_threshold = threshold;
                row.decision = decision.accepted ? "ACCEPTED" : "REJECTED";
                row.decision_basis = std::string(domain::decision_basis_name(decision.basis));
                row.reason_codes = decision.reason_codes;
                row.model_id = model && have_score ? model->model_id() : std::string();
                row.calibration_version =
                    model && have_score ? model->meta().calibration_version : std::string();
                row.inference_latency_us = scored.latency_us;
                if (decision.accepted) {
                    row.final_signal_id =
                        data::make_final_signal_id(candidate.signal_proposal_id);
                    // Kept only until the publish succeeds, so a crash in the
                    // gap re-publishes AltStrategy's proposal byte for byte
                    // rather than an approximation rebuilt from the archive.
                    row.candidate_payload = std::string(msg.data());
                }

                row.candidate_sha256 = sha256_hex(msg.data());
                const int32_t model_version = model ? model->meta().model_version : 0;
                const auto eval = to_wire_evaluation(row, *parsed, decision.basis, model_version);
                if (decision.accepted)
                    row.publication_payload = build_final(*parsed, eval, row.final_signal_id, trace_id);

                // The claim comes BEFORE any publish. This is the idempotency
                // boundary for the whole service.
                auto existing = store.claim_evaluation(row);
                if (!existing) return std::unexpected(existing.error());

                // A concurrent first claim wins with all its original evidence.
                if (*existing) return replay_existing(**existing);

                ++impl_->evaluated;
                if (decision.accepted) {
                    ++impl_->accepted;
                    if (auto r = publish_stored(data::ExistingEvaluation(row)); !r) return r;
                }
                publish_evaluation(eval, token, trace_id);
                log::info("signal_evaluated",
                          {{"signal_proposal_id", candidate.signal_proposal_id},
                           {"symbol", row.symbol},
                           {"strategy_id", candidate.strategy_id},
                           {"side", row.side},
                           {"grade", candidate.proposal_grade},
                           {"upstream_recommended", candidate.recommended_for_execution},
                           {"model_id", row.model_id},
                           {"has_quality_score", have_score},
                           {"quality_score", have_score ? scored.quality_score : -1.0},
                           {"threshold", threshold},
                           {"decision", row.decision},
                           {"basis", row.decision_basis},
                           {"reason_codes", decision.reason_codes},
                           {"missing_features", features.missing_count},
                           {"inference_latency_us", scored.latency_us}});
                return {};
            }();

            if (processed) {
                (void)msg.ack();
            } else if (input_conflict) {
                log::error("candidate_identity_refused", {{"signal_proposal_id", candidate.signal_proposal_id}});
                (void)msg.term();
            } else {
                // The decision was not durably completed. Nak so JetStream
                // redelivers; the claim row makes the retry idempotent.
                //
                // The delay grows with the delivery count. A transient blip
                // clears on the first retry, but a message that can never
                // succeed — a constraint this build does not satisfy, say —
                // would otherwise be retried every five seconds forever,
                // burying the log that says why and holding a slot in the
                // consumer's ack-pending budget. It is never dropped: losing a
                // trade signal is worse than retrying one slowly, so the
                // backoff is capped and the log escalates instead.
                const uint64_t delivered = msg.delivery_count();
                const int64_t delay_ms =
                    std::min<int64_t>(5'000LL * (delivered > 0 ? static_cast<int64_t>(delivered)
                                                               : 1),
                                      300'000LL);
                const bool persistent = delivered >= 5;
                nlohmann::json fields = {{"signal_proposal_id", candidate.signal_proposal_id},
                                         {"message", processed.error().message},
                                         {"code", processed.error().code_name()},
                                         {"delivery_count", delivered},
                                         {"retry_in_ms", delay_ms}};
                if (persistent) {
                    log::critical("signal_processing_stuck", std::move(fields));
                } else {
                    log::error("signal_processing_failed", std::move(fields));
                }
                (void)msg.nak(delay_ms);
            }
        }
        set_component_status(name(), "evaluated=" + std::to_string(impl_->evaluated) +
                                         " accepted=" + std::to_string(impl_->accepted));
    }
    log::info("evaluator_stopped", {{"evaluated", impl_->evaluated},
                                    {"accepted", impl_->accepted},
                                    {"duplicates", impl_->duplicates},
                                    {"malformed", impl_->malformed}});
}

} // namespace altsignalquality
