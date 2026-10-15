#pragma once

// decision_engine — the event-driven orchestrator (§6). It subscribes to the
// AltTech analytics components, the AltSpotter universe and the AltML forecasts,
// feeds the context_assembler, and — the instant a fresh DecisionContext is
// available for a pair — runs the full pipeline (catalogue → ensemble → adaptive
// allocator → champion + challenger ranking → three plan geometries → AltML
// evaluation → proposals), archiving and publishing every artifact. It measures
// p50/p90/p99 stage latencies, is idempotent on the context revision, and
// isolates per-pair failures so one bad pair never takes the process down.

#include "altstrategy/engine/allocator.hpp"
#include "altstrategy/engine/state_candidates.hpp"
#include "altstrategy/engine/ranking.hpp"
#include "altstrategy/engine/strategy_engine.hpp"
#include "altstrategy/engine/trade_plan.hpp"
#include "altstrategy/engine_io/context_assembler.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <tuple>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace altstrategy::messaging { class NatsClient; class Subscription; }

namespace altstrategy::engine_io {

class DecisionStore;
class OutboxPublisher;
struct OutboxMessage;

// Rolling latency samples for one stage, reported p50/p90/p99 (§6.2).
class LatencyTracker {
public:
    void record(const std::string& stage, double ms);
    void report_and_reset();

private:
    std::mutex mu_;
    std::map<std::string, std::vector<double>> samples_;
};

struct DecisionEngineConfig {
    AssemblerConfig assembler;
    StrategyEngineConfig strategy;
    AllocatorConfig allocator;
    // Plan geometry bounds. generate_plans() takes this by default argument, so
    // without carrying it here the whole `trade_plan:` config section was inert.
    PlanConfig plan;
    // Full-state decisions (state_candidates.hpp): the model acts on a market
    // state whether or not a setup detector fired.
    StateCandidateConfig state;
    std::vector<std::string> alttech_subjects = {"alttech.feature.>", "alttech.regime.>",
                                                 "alttech.structure.>", "alttech.quality.>"};
    std::vector<std::string> prediction_subjects = {"prediction.>"};
    std::vector<std::string> universe_subjects = {"evt.universe.>"};
    bool publish_intermediate = true;
    bool evaluate_all_variants = true;   // request AltML for each geometry
    // How often the engine re-reads posteriors, lifecycle and parameter sets.
    int64_t adaptive_reload_interval_ms = 5LL * 60 * 1000;
    // Attempts at archiving one candidate's decision when the failure is a
    // transient one (a dropped connection); anything else is not retried.
    int archive_attempts = 3;
    // Posteriors are used only when estimated from outcomes of at least this
    // plan-geometry version (SchedulerConfig::min_plan_geometry_version).
    int min_plan_geometry_version = 1;
};

class DecisionEngine {
public:
    // `publisher` sends what the archive transaction queued (BUG-01/02): the
    // engine itself never publishes a proposal.
    DecisionEngine(altstrategy::messaging::NatsClient& nats, DecisionStore& store,
                   OutboxPublisher& publisher, DecisionEngineConfig cfg);
    ~DecisionEngine();

    bool start();                    // load adaptive state + subscribe to inputs
    void reload_adaptive_state();    // posteriors + lifecycle overrides from store
    // Rebuild the de-duplication books (setups and states) from the archive.
    void restore_open_hypotheses(int64_t now_ms);
    // Stage latencies, plus the decisions that could not be archived and the
    // intermediate publishes the bus refused since the last report.
    void report_latency();
    // Logs `strategy_funnel` per strategy once per decision interval and resets.
    void report_strategy_funnel();

    // Exposed for the integration path / diagnostics.
    void run_for_context(const DecisionContext& ctx);

    // Where closed 1m candles come from when the session VWAP has to be
    // rebuilt after a restart: (pair, session_start_ms, until_ms) -> minutes.
    using SessionSeedSource =
        std::function<std::vector<SessionMinute>(int64_t, int64_t, int64_t)>;
    void set_session_seed_source(SessionSeedSource src) { session_seed_ = std::move(src); }

    // Where the venue's price grid comes from (MarketReader::read_price_ticks),
    // re-read with the adaptive state. nullopt from it keeps the last table.
    using PriceTickSource = std::function<std::optional<PriceTickTable>()>;
    void set_price_tick_source(PriceTickSource src) { price_ticks_source_ = std::move(src); }

private:
    SessionSeedSource session_seed_;
    PriceTickSource price_ticks_source_;
    // The grid in force at the context's event time, onto the context.
    void attach_price_tick(DecisionContext& ctx);
    // Best-effort intermediate publish: counted, never retried.
    void publish_intermediate(const std::string& subject, const std::string& bytes);
    // The context first; false (logged, counted) means no decision of it may
    // be archived or published.
    bool archive_context(const DecisionContext& ctx, size_t candidates);
    // One candidate's decision and its publications, retried only on a
    // transient failure; false means it was archived -- and published -- nowhere.
    bool archive_decision(const StrategyCandidate& c, const std::vector<TradePlanVariant>& plans,
                          const std::vector<TradeSignalProposal>& proposals,
                          const std::vector<OutboxMessage>& publications);
    void on_alttech(std::string_view bytes);
    void process_candidates(const DecisionContext& ctx, std::vector<StrategyCandidate>& candidates,
                            int64_t t_ms, std::chrono::steady_clock::time_point t_ctx);
    bool admit_state(int64_t pair, int32_t horizon, Side side, int64_t event_ms);
    void on_forecast(std::string_view bytes);
    void on_universe(std::string_view bytes);
    [[nodiscard]] std::string symbol_group_of(int64_t pair) const;

    altstrategy::messaging::NatsClient& nats_;
    DecisionStore& store_;
    OutboxPublisher& publisher_;
    DecisionEngineConfig cfg_;

    ContextAssembler assembler_;
    StrategyEngine engine_;
    // One open hypothesis per (pair, setup, side); see strategy_engine.hpp.
    HypothesisBook hypotheses_;
    AdaptiveAllocator allocator_;
    RankingPolicy champion_;
    RankingPolicy challenger_;

    std::mutex state_mu_;
    // Adaptive state was loaded once in start() and never again, so every
    // posterior the maintenance scheduler recomputed, every lifecycle transition
    // it recorded and every calibrated parameter set it installed stayed
    // invisible to the running engine until the next restart.
    std::atomic<int64_t> last_reload_ms_{0};
    std::atomic<int64_t> last_reject_log_ms_{0};
    PosteriorTable posteriors_;
    CatalogueState catalogue_state_;
    PriceTickTable price_ticks_;

    // Decisions that could not be archived, rankings that could not be (their
    // proposals then name none), and intermediate publishes the bus refused --
    // reported with the latency figures.
    std::atomic<int64_t> archive_failed_{0};
    std::atomic<int64_t> rankings_not_archived_{0};
    std::atomic<int64_t> intermediate_publish_failed_{0};

    LatencyTracker latency_;
    std::mutex funnel_mu_;
    StrategyFunnel funnel_;
    // One open state thesis per (pair, horizon, side): when the last one was
    // raised. A state that keeps its edge minute after minute is one thesis
    // held for its horizon, not a new signal every bar.
    std::mutex state_seen_mu_;
    std::map<std::tuple<int64_t, int32_t, int>, int64_t> state_seen_;
    int64_t funnel_since_ms_ = 0;
    std::vector<std::unique_ptr<altstrategy::messaging::Subscription>> subs_;
};

} // namespace altstrategy::engine_io
