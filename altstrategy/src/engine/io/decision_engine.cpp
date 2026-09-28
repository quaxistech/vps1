#include "altstrategy/engine_io/decision_engine.hpp"

#include "altstrategy/engine/ensemble.hpp"
#include "altstrategy/engine/proposal.hpp"
#include "altstrategy/engine/trade_plan.hpp"
#include "altstrategy/engine_io/decision_store.hpp"
#include "altstrategy/engine_io/wire_codec.hpp"
#include "altstrategy/logging/logger.hpp"
#include "altstrategy/messaging/nats_client.hpp"
#include "altstrategy/messaging/nats_subjects.hpp"

#include <algorithm>
#include <chrono>

namespace altstrategy::engine_io {

namespace {
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
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
        log::info("strategy_funnel", {{"strategy", id},
                                      {"window_s", (t_ms - since) / 1000},
                                      {"contexts", c.contexts},
                                      {"ineligible", ineligible},
                                      {"evaluated", c.evaluated},
                                      {"no_setup", c.no_setup},
                                      {"geometry_rejected", c.geometry_rejected},
                                      {"horizon_refused", c.horizon_refused},
                                      {"repeat_within_horizon", c.repeat_within_horizon},
                                      {"candidates", c.candidates},
                                      {"econ", econ}});
    }
}

DecisionEngine::DecisionEngine(altstrategy::messaging::NatsClient& nats, DecisionStore& store,
                               DecisionEngineConfig cfg)
    : nats_(nats), store_(store), cfg_(std::move(cfg)),
      assembler_(cfg_.assembler), engine_(cfg_.strategy), allocator_(cfg_.allocator),
      champion_(default_champion_policy()), challenger_(default_challenger_policy()) {}

DecisionEngine::~DecisionEngine() = default;

void DecisionEngine::reload_adaptive_state() {
    auto post = store_.load_posteriors();
    auto lc = store_.load_lifecycle_overrides();
    auto params = store_.load_param_sets();
    std::lock_guard<std::mutex> lk(state_mu_);
    posteriors_ = std::move(post);
    catalogue_state_.lifecycle = std::move(lc);
    catalogue_state_.params = std::move(params);
}

bool DecisionEngine::start() {
    reload_adaptive_state();

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
    const auto admit = [&](int32_t horizon, Side side) { return admit_state(snap->canonical_pair_id, horizon, side, snap->event_time_ms); };
    auto states = state_candidates(*snap, t_ms, cfg_.strategy.econ, cfg_.state, {}, admit,
                                   f->view.horizon_minutes);
    if (states.empty()) return;
    try {
        process_candidates(*snap, states, t_ms, std::chrono::steady_clock::now());
    } catch (const std::exception& e) {
        log::error("state_decision_exception", {{"pair", snap->canonical_pair_id}, {"message", e.what()}});
    }
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
    // published once per its horizon (HypothesisBook).
    const auto admit = [&](const std::string& strategy_id, Side side, int64_t horizon_ms) {
        return hypotheses_.admit(ctx.canonical_pair_id, strategy_id, side, ctx.event_time_ms, horizon_ms);
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
    store_.save_context_snapshot(ctx);
    if (candidates.empty()) return;
    process_candidates(ctx, candidates, t_ms, t_ctx);
}

// Everything from the candidates to published proposals, shared by the
// context path (every decision bar: setups + full-state) and the full-state
// path between decision bars (on_forecast).
void DecisionEngine::process_candidates(const DecisionContext& ctx,
                                        std::vector<StrategyCandidate>& candidates, int64_t t_ms,
                                        std::chrono::steady_clock::time_point t_ctx) {
    PosteriorTable posteriors;
    { std::lock_guard<std::mutex> lk(state_mu_); posteriors = posteriors_; }
    // 2) adaptive allocator + archive every candidate
    const std::string sg = symbol_group_of(ctx.canonical_pair_id);
    for (auto& c : candidates) {
        allocator_.allocate(c, sg, posteriors);
        store_.save_candidate(c);
        if (cfg_.publish_intermediate) {
            EnvelopeMeta m; m.canonical_pair_id = ctx.canonical_pair_id; m.symbol = ctx.symbol;
            m.event_type = "candidate"; m.message_id = c.candidate_id;
            nats_.publish(altstrategy::nats_subjects::candidate(ctx.symbol.empty() ? std::to_string(ctx.canonical_pair_id) : ctx.symbol,
                                                             c.strategy_id), encode_candidate(c, m));
        }
    }

    // 3) ensemble
    const auto t_rank0 = std::chrono::steady_clock::now();
    const auto ens = compute_ensemble(candidates);

    // 4) champion + challenger ranking
    const auto champ = rank(candidates, ctx, ens, champion_, t_ms);
    const auto chall = rank(candidates, ctx, ens, challenger_, t_ms);
    latency_.record("candidates_to_ranking", ms_since(t_rank0));
    store_.save_ranking(champ);
    store_.save_ranking(chall);
    if (cfg_.publish_intermediate) {
        EnvelopeMeta m; m.canonical_pair_id = ctx.canonical_pair_id; m.symbol = ctx.symbol;
        m.event_type = "ranking"; m.message_id = champ.ranking_id;
        const std::string sym = ctx.symbol.empty() ? std::to_string(ctx.canonical_pair_id) : ctx.symbol;
        nats_.publish(altstrategy::nats_subjects::ranking(sym), encode_ranking(champ, m));
        nats_.publish(altstrategy::nats_subjects::ensemble(sym), encode_ensemble(ens, ctx.canonical_pair_id, m));
    }

    // 5) EVERY candidate is a VPS1 signal.
    //
    // Only the champion's recommended candidate used to be planned and
    // proposed, and the champion ranked on the economic estimate: a context
    // with a breakout and a VWAP reclaim published one of them, chosen by
    // whether a model said it paid after costs -- VPS1 selecting on VPS2's
    // question. Every valid setup is now published with its market view; the
    // rankings above stay on record as diagnostics and select nothing.
    const std::string sym = ctx.symbol.empty() ? std::to_string(ctx.canonical_pair_id) : ctx.symbol;
    for (const auto& cand : candidates) {
        const auto t_plan0 = std::chrono::steady_clock::now();
        const auto plans = generate_plans(cand, ctx, t_ms, cfg_.plan);
        latency_.record("candidate_to_plans", ms_since(t_plan0));
        std::vector<std::string> alt_ids;
        for (const auto& v : plans) {
            store_.save_plan_variant(v);
            if (v.profile != PlanProfile::balanced) alt_ids.push_back(v.variant_id);
        }
        for (const auto& v : plans) {
            const auto ml = cfg_.evaluate_all_variants || v.profile == PlanProfile::balanced
                                ? ml_from_forecast(ctx.market_forecast) : MlEvaluation{};
            const bool is_main = v.profile == PlanProfile::balanced;
            ProposalInputs in{cand, v, ml, ens, ctx, champ.ranking_id,
                              is_main ? alt_ids : std::vector<std::string>{}, is_main,
                              is_main ? 1 : (v.profile == PlanProfile::aggressive ? 2 : 3)};
            const auto t_prop = std::chrono::steady_clock::now();
            const auto proposal = build_proposal(in, t_ms);
            latency_.record("plan_to_proposal", ms_since(t_prop));
            latency_.record("context_to_proposal", ms_since(t_ctx));
            store_.save_proposal(proposal);
            // INVALID proposals are archived but never published (§13).
            if (proposal.proposal_grade != Grade::invalid) {
                EnvelopeMeta m; m.canonical_pair_id = ctx.canonical_pair_id; m.symbol = ctx.symbol;
                m.event_type = "trade_signal_proposal"; m.message_id = proposal.signal_proposal_id;
                const std::string bytes = encode_proposal(proposal, m);
                nats_.publish(altstrategy::nats_subjects::trade_proposal(sym), bytes);
                if (is_main) nats_.publish(altstrategy::nats_subjects::signal_proposed(sym), bytes);
            }
        }
    }
}

} // namespace altstrategy::engine_io
