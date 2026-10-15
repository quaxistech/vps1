#include "altstrategy/engine_io/decision_engine.hpp"

#include "altstrategy/engine/ensemble.hpp"
#include "altstrategy/engine/proposal.hpp"
#include "altstrategy/engine/trade_plan.hpp"
#include "altstrategy/engine_io/decision_store.hpp"
#include "altstrategy/engine_io/outbox_publisher.hpp"
#include "altstrategy/engine_io/wire_codec.hpp"
#include "altstrategy/logging/logger.hpp"
#include "altstrategy/messaging/nats_client.hpp"
#include "altstrategy/messaging/nats_subjects.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <thread>

namespace altstrategy::engine_io {

namespace {
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
// An archive write, retried while -- and only while -- its failure is transient
// (a pooled connection the database dropped), at most `attempts` times; a
// constraint violation fails the same way every time.
template <class Write>
Result<void> retry_transient(int attempts, Write&& write) {
    for (int attempt = 1;; ++attempt) {
        auto r = write();
        if (r || !r.error().retryable || attempt >= attempts) return r;
        std::this_thread::sleep_for(std::chrono::milliseconds(100 * attempt));
    }
}
} // namespace

void LatencyTracker::record(const std::string& stage, double ms) {
    std::lock_guard<std::mutex> lk(mu_);
    samples_[stage].push_back(ms);
}
void LatencyTracker::report_and_reset() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [stage, v] : samples_) {
        if (v.empty()) continue;
        std::sort(v.begin(), v.end());
        auto q = [&](double p) {
            return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
        };
        log::info("latency", {{"stage", stage}, {"n", static_cast<int64_t>(v.size())},
                              {"p50_ms", q(0.50)}, {"p90_ms", q(0.90)}, {"p99_ms", q(0.99)}});
        v.clear();
    }
}

void DecisionEngine::report_strategy_funnel() {
    // One decision bar: each window then holds roughly one context per pair.
    constexpr int64_t kFunnelIntervalMs = 15LL * 60 * 1000;
    const int64_t t_ms = now_ms();
    StrategyFunnel f;
    int64_t since = 0;
    {
        std::lock_guard<std::mutex> lk(funnel_mu_);
        if (funnel_since_ms_ == 0 || t_ms - funnel_since_ms_ < kFunnelIntervalMs) return;
        f = std::move(funnel_);
        funnel_ = {};
        since = funnel_since_ms_;
        funnel_since_ms_ = t_ms;
    }
    for (const auto& [id, c] : f.by_strategy) {
        nlohmann::json ineligible = nlohmann::json::object();
        for (const auto& [reason, n] : c.ineligible) ineligible[reason] = n;
        nlohmann::json econ = nlohmann::json::object();
        for (const auto& [state, n] : c.econ) econ[state] = n;
        const auto buckets = [](const std::array<int64_t, 4>& b) {
            return nlohmann::json{{"lt_0_5_atr", b[0]}, {"0_5_to_1_atr", b[1]}, {"1_to_2_atr", b[2]}, {"ge_2_atr", b[3]}};
        };
        log::info("strategy_funnel", {{"strategy", id},
                                      {"window_s", (t_ms - since) / 1000},
                                      {"contexts", c.contexts},
                                      {"ineligible", ineligible},
                                      {"evaluated", c.evaluated},
                                      {"no_setup", c.no_setup},
                                      {"geometry_rejected", c.geometry_rejected},
                                      {"horizon_refused", c.horizon_refused},
                                      {"repeat_within_horizon", c.repeat_within_horizon},
                                      {"repeat_by_reset", buckets(c.repeat_by_reset_atr)},
                                      {"readmitted_by_reset", buckets(c.readmitted_by_reset_atr)},
                                      {"candidates", c.candidates},
                                      {"econ", econ}});
    }
}

DecisionEngine::DecisionEngine(altstrategy::messaging::NatsClient& nats, DecisionStore& store,
                               OutboxPublisher& publisher, DecisionEngineConfig cfg)
    : nats_(nats), store_(store), publisher_(publisher), cfg_(std::move(cfg)),
      assembler_(cfg_.assembler), engine_(cfg_.strategy), allocator_(cfg_.allocator),
      champion_(default_champion_policy()), challenger_(default_challenger_policy()) {}

DecisionEngine::~DecisionEngine() = default;

void DecisionEngine::report_latency() {
    latency_.report_and_reset();
    const int64_t archive_failed = archive_failed_.exchange(0);
    const int64_t rankings_failed = rankings_not_archived_.exchange(0);
    const int64_t publish_failed = intermediate_publish_failed_.exchange(0);
    if (archive_failed > 0 || rankings_failed > 0 || publish_failed > 0)
        log::warn("decision_output_failures", {{"decisions_not_archived", archive_failed},
                                               {"rankings_not_archived", rankings_failed},
                                               {"intermediate_publishes_failed", publish_failed}});
}

void DecisionEngine::reload_adaptive_state() {
    auto post = store_.load_posteriors(cfg_.min_plan_geometry_version);
    auto lc = store_.load_lifecycle_overrides();
    auto params = store_.load_param_sets();
    std::optional<PriceTickTable> ticks;
    if (price_ticks_source_) ticks = price_ticks_source_();
    std::lock_guard<std::mutex> lk(state_mu_);
    posteriors_ = std::move(post);
    catalogue_state_.lifecycle = std::move(lc);
    catalogue_state_.params = std::move(params);
    // A failed read keeps the grid already known; it never empties it.
    if (ticks) price_ticks_ = std::move(*ticks);
}

void DecisionEngine::attach_price_tick(DecisionContext& ctx) {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (const auto* t = price_tick_at(price_ticks_, ctx.canonical_pair_id, ctx.event_time_ms)) {
        ctx.price_tick = t->tick;
        ctx.instrument_spec_version = t->version;
    }
}

void DecisionEngine::publish_intermediate(const std::string& subject, const std::string& bytes) {
    // Candidates, rankings and ensembles are observability: nothing downstream
    // depends on them, so they stay a core publish -- but a refused one is
    // counted rather than discarded unseen.
    if (!nats_.publish(subject, bytes)) intermediate_publish_failed_.fetch_add(1, std::memory_order_relaxed);
}

bool DecisionEngine::start() {
    reload_adaptive_state();
    restore_open_hypotheses(now_ms());

    const auto sub = [&](const std::string& subject, void (DecisionEngine::*fn)(std::string_view)) {
        auto s = nats_.subscribe(subject, [this, fn](altstrategy::messaging::Msg m) {
            try { (this->*fn)(m.data()); }
            catch (const std::exception& e) { log::error("input_handler_exception", {{"message", e.what()}}); }
        });
        if (!s) { log::error("subscribe_failed", {{"subject", subject}}); return false; }
        subs_.push_back(std::make_unique<altstrategy::messaging::Subscription>(std::move(*s)));
        return true;
    };
    bool ok = true;
    for (const auto& s : cfg_.alttech_subjects) ok = sub(s, &DecisionEngine::on_alttech) && ok;
    for (const auto& s : cfg_.prediction_subjects) ok = sub(s, &DecisionEngine::on_forecast) && ok;
    for (const auto& s : cfg_.universe_subjects) ok = sub(s, &DecisionEngine::on_universe) && ok;
    return ok;
}

std::string DecisionEngine::symbol_group_of(int64_t) const { return "alts"; }

void DecisionEngine::on_alttech(std::string_view bytes) {
    auto p = parse_alttech(bytes);
    if (!p || p->kind == AltTechKind::none) return;
    // Rebuild the session VWAP from candles before the first live window of a
    // session this process has not accumulated (a restart mid-day).
    if (p->kind == AltTechKind::bitget_microstructure && session_seed_) {
        if (const auto session = assembler_.session_vwap_seed_needed(
                p->canonical_pair_id, p->bar_open_time_ms, p->window_label)) {
            std::vector<SessionMinute> minutes;
            try {
                minutes = session_seed_(p->canonical_pair_id, *session, p->bar_open_time_ms);
            } catch (const std::exception& e) {
                log::warn("session_vwap_seed_failed", {{"pair", p->canonical_pair_id}, {"message", e.what()}});
            }
            assembler_.seed_session_vwap(p->canonical_pair_id, *session, minutes);
            log::info("session_vwap_seeded", {{"pair", p->canonical_pair_id},
                                              {"minutes", static_cast<int64_t>(minutes.size())}});
        }
    }
    assembler_.ingest_alttech(*p);
    // Context drivers: a fresh Bitget feature vector, a new global regime, or a
    // new per-venue Bitget regime (fallback while AltTech's cross-venue
    // consensus regime is not yet published) can complete a context. Try to
    // build + run immediately (event-driven, §6).
    // A microstructure window completes a context that was held back waiting
    // for it (the freshness race; see ContextAssembler::try_build).
    // An information venue's vector completes a context held back waiting for
    // the decision bar's venues; any microstructure window also retries it, so
    // that a venue that never comes is given up on right after the wait.
    const bool micro_retry =
        (p->kind == AltTechKind::bitget_microstructure &&
         (assembler_.awaiting_microstructure(p->canonical_pair_id) ||
          assembler_.awaiting_information_venues(p->canonical_pair_id))) ||
        (p->kind == AltTechKind::reference_features &&
         assembler_.awaiting_information_venues(p->canonical_pair_id));
    if (p->kind == AltTechKind::bitget_features || p->kind == AltTechKind::global_regime ||
        p->kind == AltTechKind::bitget_regime || micro_retry) {
        if (auto ctx = assembler_.try_build(p->canonical_pair_id, now_ms())) {
            attach_price_tick(*ctx);
            try { run_for_context(*ctx); }
            catch (const std::exception& e) {
                log::error("pipeline_exception", {{"pair", p->canonical_pair_id}, {"message", e.what()}});
            }
        }
    }
}

void DecisionEngine::on_forecast(std::string_view bytes) {
    auto f = parse_altml_forecast(bytes);
    if (!f) return;
    assembler_.ingest_forecast(*f);
    // Full-state decisions between decision bars: AltML re-estimates every
    // closed 1m bar, and a state whose estimate clears the model's validated
    // margin must not wait up to a quarter of an hour for the next 15m
    // context. The arriving horizon is put to the policy on the current state.
    if (!cfg_.state.enabled || !f->view.econ || f->view.horizon_minutes <= 0) return;
    const int64_t t_ms = now_ms();
    auto snap = assembler_.state_snapshot(f->canonical_pair_id, t_ms);
    if (!snap) return;
    attach_price_tick(*snap);
    const auto admit = [&](int32_t horizon, Side side) { return admit_state(snap->canonical_pair_id, horizon, side, snap->event_time_ms); };
    auto states = state_candidates(*snap, t_ms, cfg_.strategy.econ, cfg_.state, {}, admit,
                                   f->view.horizon_minutes);
    if (states.empty() || !archive_context(*snap, states.size())) return;
    try {
        process_candidates(*snap, states, t_ms, std::chrono::steady_clock::now());
    } catch (const std::exception& e) {
        log::error("state_decision_exception", {{"pair", snap->canonical_pair_id}, {"message", e.what()}});
    }
}

void DecisionEngine::restore_open_hypotheses(int64_t now) {
    // Both de-duplication books lived in memory only, so a restart re-issued
    // every hypothesis still open as a new signal. The archive holds every
    // candidate admitted, so the books are rebuilt from it: a candidate's
    // setup time plus its horizon is exactly when it closes.
    const auto open = store_.load_open_hypotheses(now);
    if (!open) {
        log::error("open_hypotheses_not_restored",
                   {{"message", "archive read failed: a restart may re-issue open hypotheses"}});
        return;
    }
    int64_t setups = 0, states = 0;
    for (const auto& h : *open) {
        if (h.strategy_id == kStateStrategyId) {
            // parameter_set_id is "h<horizon>" for a state candidate.
            int32_t hz = 0;
            if (h.parameter_set_id.size() > 1)
                std::from_chars(h.parameter_set_id.data() + 1, h.parameter_set_id.data() + h.parameter_set_id.size(), hz);
            if (hz <= 0) continue;
            std::lock_guard<std::mutex> lk(state_seen_mu_);
            auto& seen = state_seen_[std::make_tuple(h.canonical_pair_id, hz, static_cast<int>(h.side))];
            seen = std::max(seen, h.setup_time_ms);
            ++states;
        } else {
            hypotheses_.restore(h.canonical_pair_id, h.strategy_id, h.side, h.open_until_ms, h.ref_price, h.atr);
            ++setups;
        }
    }
    log::info("open_hypotheses_restored", {{"setups", setups}, {"states", states}});
}

bool DecisionEngine::admit_state(int64_t pair, int32_t horizon, Side side, int64_t event_ms) {
    std::lock_guard<std::mutex> lk(state_seen_mu_);
    const auto key = std::make_tuple(pair, horizon, static_cast<int>(side));
    const auto it = state_seen_.find(key);
    const int64_t hold = static_cast<int64_t>(horizon) * 60'000;
    if (it != state_seen_.end() && event_ms - it->second < hold) return false;
    state_seen_[key] = event_ms;
    return true;
}

void DecisionEngine::on_universe(std::string_view bytes) {
    if (auto p = parse_alttech(bytes); p && p->canonical_pair_id != 0 && !p->universe_status.empty())
        assembler_.set_universe(p->canonical_pair_id, p->universe_status);
}

void DecisionEngine::run_for_context(const DecisionContext& ctx) {
    const int64_t t_ms = now_ms();
    const auto t_ctx = std::chrono::steady_clock::now();

    // Pick up whatever the maintenance scheduler has learned since the last
    // refresh. Cheap: a timestamp compare on the hot path, a DB read only once
    // per interval.
    // Claim the refresh with a compare-exchange rather than load-then-store:
    // contexts are dispatched from NATS callbacks, so a plain check lets two
    // callers both pass and both hit the database for the same interval.
    int64_t due = last_reload_ms_.load(std::memory_order_relaxed);
    if (t_ms - due >= cfg_.adaptive_reload_interval_ms &&
        last_reload_ms_.compare_exchange_strong(due, t_ms, std::memory_order_relaxed)) {
        reload_adaptive_state();
    }

    CatalogueState state;
    PosteriorTable posteriors;
    { std::lock_guard<std::mutex> lk(state_mu_); state = catalogue_state_; posteriors = posteriors_; }

    // 1) candidates
    StrategyFunnel funnel;
    // One open hypothesis per (pair, setup, side): a persisting state is
    // published once per its horizon (HypothesisBook). The reset depth since
    // the previous one opened is counted, never acted on (DESIGN-04 shadow).
    const double px = ctx.last_price();
    hypotheses_.observe(ctx.canonical_pair_id, px);
    const auto admit = [&](const std::string& strategy_id, Side side, int64_t horizon_ms) {
        const auto a = hypotheses_.admit(ctx.canonical_pair_id, strategy_id, side, ctx.event_time_ms, horizon_ms,
                                         px, ctx.atr());
        if (a.reset_atr >= 0) {
            auto& fc = funnel.by_strategy[strategy_id];
            (a.admitted ? fc.readmitted_by_reset_atr : fc.repeat_by_reset_atr)[reset_bucket(a.reset_atr)]++;
        }
        return a.admitted;
    };
    auto candidates = engine_.run(ctx, t_ms, state, &funnel, admit);
    latency_.record("context_to_candidates", ms_since(t_ctx));
    {
        std::lock_guard<std::mutex> lk(funnel_mu_);
        if (funnel_since_ms_ == 0) funnel_since_ms_ = t_ms;
        funnel_.merge(funnel);
    }
    // 1b) full-state candidates: the economic policy over every horizon and
    // side of this state, tagged with the setups above (state_candidates.hpp)
    {
        const auto admit = [&](int32_t horizon, Side side) {
            return admit_state(ctx.canonical_pair_id, horizon, side, ctx.event_time_ms);
        };
        auto states = state_candidates(ctx, t_ms, cfg_.strategy.econ, cfg_.state, candidates, admit);
        for (auto& c : states) candidates.push_back(std::move(c));
    }
    // Every context is archived, candidates or not (drift normalises by them).
    if (!archive_context(ctx, candidates.size()) || candidates.empty()) return;
    process_candidates(ctx, candidates, t_ms, t_ctx);
}

bool DecisionEngine::archive_context(const DecisionContext& ctx, size_t candidates) {
    // A transient failure -- a pooled connection the database dropped, the
    // first use after a restart -- is retried like a decision's archive.
    if (retry_transient(cfg_.archive_attempts, [&] { return store_.save_context_snapshot(ctx); })) return true;
    // The context is the root of every record a decision makes (BUG-01).
    // Without it nothing is published: AltSignalQuality reads the archived
    // context for every proposal it receives, and a signal that cannot be
    // traced to its inputs is the failure this archive exists to prevent.
    if (candidates > 0) {
        archive_failed_.fetch_add(static_cast<int64_t>(candidates), std::memory_order_relaxed);
        log::error("decision_context_not_archived", {{"context_id", ctx.context_id},
                                                     {"pair", ctx.canonical_pair_id},
                                                     {"candidates_dropped", static_cast<int64_t>(candidates)}});
    }
    return false;
}

bool DecisionEngine::archive_decision(const StrategyCandidate& c, const std::vector<TradePlanVariant>& plans,
                                      const std::vector<TradeSignalProposal>& proposals,
                                      const std::vector<OutboxMessage>& publications) {
    const auto r = retry_transient(cfg_.archive_attempts,
                                   [&] { return store_.archive_decision(c, plans, proposals, publications); });
    if (r) return true;
    archive_failed_.fetch_add(1, std::memory_order_relaxed);
    log::error("decision_not_archived", {{"candidate_id", c.candidate_id},
                                         {"strategy", c.strategy_id},
                                         {"retryable", r.error().retryable},
                                         {"message", r.error().message}});
    return false;
}

// Everything from the candidates to published proposals, shared by the
// context path (every decision bar: setups + full-state) and the full-state
// path between decision bars (on_forecast).
void DecisionEngine::process_candidates(const DecisionContext& ctx,
                                        std::vector<StrategyCandidate>& candidates, int64_t t_ms,
                                        std::chrono::steady_clock::time_point t_ctx) {
    PosteriorTable posteriors;
    { std::lock_guard<std::mutex> lk(state_mu_); posteriors = posteriors_; }
    const std::string sym = ctx.symbol.empty() ? std::to_string(ctx.canonical_pair_id) : ctx.symbol;
    // 2) adaptive allocator
    const std::string sg = symbol_group_of(ctx.canonical_pair_id);
    for (auto& c : candidates) allocator_.allocate(c, sg, posteriors);

    // 3) ensemble
    const auto t_rank0 = std::chrono::steady_clock::now();
    const auto ens = compute_ensemble(candidates);

    // 4) champion + challenger ranking -- diagnostics, archived and gating nothing
    const auto champ = rank(candidates, ctx, ens, champion_, t_ms);
    const auto chall = rank(candidates, ctx, ens, challenger_, t_ms);
    latency_.record("candidates_to_ranking", ms_since(t_rank0));
    // Every proposal names the champion's ranking. One that could not be
    // archived is not named -- a reference to a record that does not exist is
    // the gap the archive is there to close.
    const bool ranked = retry_transient(cfg_.archive_attempts, [&] { return store_.save_ranking(champ); }).has_value();
    const bool challenger_ranked =
        retry_transient(cfg_.archive_attempts, [&] { return store_.save_ranking(chall); }).has_value();
    rankings_not_archived_.fetch_add((ranked ? 0 : 1) + (challenger_ranked ? 0 : 1), std::memory_order_relaxed);
    const std::string ranking_id = ranked ? champ.ranking_id : std::string{};

    // 5) EVERY candidate is a VPS1 signal.
    //
    // Only the champion's recommended candidate used to be planned and
    // proposed, and the champion ranked on the economic estimate: a context
    // with a breakout and a VWAP reclaim published one of them, chosen by
    // whether a model said it paid after costs -- VPS1 selecting on VPS2's
    // question. Every valid setup is now published with its market view; the
    // rankings above stay on record as diagnostics and select nothing.
    //
    // Each candidate's decision -- candidate, plan variants, proposals and the
    // publications they owe -- is archived in ONE transaction and published by
    // the outbox only after it commits (BUG-01/02). A candidate that cannot be
    // archived is published nowhere.
    size_t archived = 0;
    for (const auto& cand : candidates) {
        const auto t_plan0 = std::chrono::steady_clock::now();
        const auto plans = generate_plans(cand, ctx, t_ms, cfg_.plan);
        latency_.record("candidate_to_plans", ms_since(t_plan0));
        std::vector<std::string> alt_ids;
        for (const auto& v : plans)
            if (v.profile != PlanProfile::balanced) alt_ids.push_back(v.variant_id);
        std::vector<TradeSignalProposal> proposals;
        std::vector<OutboxMessage> publications;
        for (const auto& v : plans) {
            const auto ml = cfg_.evaluate_all_variants || v.profile == PlanProfile::balanced
                                ? ml_from_forecast(ctx.market_forecast) : MlEvaluation{};
            const bool is_main = v.profile == PlanProfile::balanced;
            ProposalInputs in{cand, v, ml, ens, ctx, ranking_id,
                              is_main ? alt_ids : std::vector<std::string>{}, is_main,
                              is_main ? 1 : (v.profile == PlanProfile::aggressive ? 2 : 3)};
            const auto t_prop = std::chrono::steady_clock::now();
            auto proposal = build_proposal(in, t_ms);
            latency_.record("plan_to_proposal", ms_since(t_prop));
            latency_.record("context_to_proposal", ms_since(t_ctx));
            // INVALID proposals are archived but never published (§13).
            if (proposal.proposal_grade != Grade::invalid) {
                EnvelopeMeta m; m.canonical_pair_id = ctx.canonical_pair_id; m.symbol = ctx.symbol;
                m.event_type = "trade_signal_proposal"; m.message_id = proposal.signal_proposal_id;
                const std::string bytes = encode_proposal(proposal, m);
                // Both subjects share one stream, whose duplicate window is
                // keyed on Nats-Msg-Id: the ids differ by subject, or the
                // second message would be dropped as a duplicate of the first.
                const auto queue = [&](const std::string& kind, const std::string& subject) {
                    publications.push_back(OutboxMessage{kind + ":" + proposal.signal_proposal_id, kind,
                                                         subject, bytes, proposal.signal_proposal_id,
                                                         cand.candidate_id, t_ms});
                };
                queue("trade_proposal", altstrategy::nats_subjects::trade_proposal(sym));
                if (is_main) queue("signal_proposed", altstrategy::nats_subjects::signal_proposed(sym));
            }
            proposals.push_back(std::move(proposal));
        }
        const auto t_arch = std::chrono::steady_clock::now();
        if (!archive_decision(cand, plans, proposals, publications)) continue;
        latency_.record("decision_archive", ms_since(t_arch));
        ++archived;
        if (!publications.empty()) publisher_.notify();
        if (cfg_.publish_intermediate) {
            EnvelopeMeta m; m.canonical_pair_id = ctx.canonical_pair_id; m.symbol = ctx.symbol;
            m.event_type = "candidate"; m.message_id = cand.candidate_id;
            publish_intermediate(altstrategy::nats_subjects::candidate(sym, cand.strategy_id),
                                 encode_candidate(cand, m));
        }
    }
    // The ranking and ensemble describe the candidates above; with none of
    // them archived they would describe records that do not exist.
    if (cfg_.publish_intermediate && archived > 0) {
        EnvelopeMeta m; m.canonical_pair_id = ctx.canonical_pair_id; m.symbol = ctx.symbol;
        m.event_type = "ranking"; m.message_id = champ.ranking_id;
        if (ranked) publish_intermediate(altstrategy::nats_subjects::ranking(sym), encode_ranking(champ, m));
        publish_intermediate(altstrategy::nats_subjects::ensemble(sym), encode_ensemble(ens, ctx.canonical_pair_id, m));
    }
}

} // namespace altstrategy::engine_io
