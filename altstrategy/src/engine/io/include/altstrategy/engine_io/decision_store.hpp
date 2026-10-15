#pragma once

// decision_store — persistence + signal_archive (§16, §33). Every valid candidate,
// ensemble, ranking, plan variant, proposal, state transition, point-in-time
// context, outcome, counterfactual, attribution, adaptive posterior, drift and
// lifecycle event is written to the altstrategy schema. Writes are idempotent on
// the natural key so NATS redelivery / restart never duplicates (§34). INVALID
// proposals are archived (audit) but flagged so they never enter the training set.

#include "altstrategy/engine/allocator.hpp"
#include "altstrategy/engine/decision_types.hpp"
#include "altstrategy/engine/outcomes.hpp"
#include "altstrategy/engine/strategy.hpp"

#include "altstrategy/engine/adapt.hpp"

#include "altstrategy/common/error.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altstrategy::persistence { class PgPool; }

namespace altstrategy::engine_io {

using namespace altstrategy::engine;

// A proposal awaiting outcome maturation, with the geometry the tracker needs.
// One resolved trade, flattened for the posterior estimator: the identity of
// what produced it, the market state it was taken in, and what it returned.
struct OutcomeSample {
    std::string strategy_id;
    std::string strategy_version;
    std::string parameter_set_id;
    std::string regime;        // global regime at decision time
    std::string symbol_group;
    int64_t canonical_pair_id = 0;
    Side side = Side::unspecified;
    HoldingClass holding_class = HoldingClass::unspecified;
    int64_t resolved_at_ms = 0;
    double net_return = 0;
};

struct PendingProposal {
    std::string signal_proposal_id;
    std::string candidate_id;
    int64_t canonical_pair_id = 0;
    Side side = Side::unspecified;
    EntryType entry_type = EntryType::limit;
    double entry_price = 0, trigger_price = 0, stop_loss_price = 0, take_profit_price = 0;
    int64_t created_at_ms = 0, entry_fill_timeout_ms = 0, valid_until_ms = 0, time_stop_at_ms = 0;
    // The round-trip cost this plan was priced with, and which definition
    // produced it. Defaults describe the legacy rows, not a guess.
    double cost_bps = 8.0;
    int cost_model_version = 1;
    Grade grade = Grade::normal;
    // Carried so a resolved outcome can be attributed without a second lookup.
    std::string ranking_id, strategy_id, strategy_version;
    // Whether a model produced an expected return at all; see
    // TradeSignalProposal::edge_estimated. The cost sits in cost_bps above.
    double expected_net_return = 0;
    bool edge_estimated = false;
    std::string regime_at_creation;
};

// One candidate's full set of plan variants, each with its reconstructed
// outcome, so attribution and variant-regret are computed from one read.
struct VariantOutcome {
    std::string signal_proposal_id;
    int plan_version = 1;
    bool needs_attribution = false;
    double round_trip_cost_bps = 0, expected_net_return = 0;
    bool edge_estimated = false;
    MarketOutcome outcome;
};

struct AttributionWork {
    std::string candidate_id;
    std::string ranking_id;
    int64_t canonical_pair_id = 0;
    std::string regime_at_creation;
    std::string regime_at_resolution;
    std::vector<VariantOutcome> variants;
};

// One publication a committed record owes the bus: a row of
// altstrategy.publication_outbox, written in the transaction that archives the
// record and sent by OutboxPublisher with `message_id` as its Nats-Msg-Id.
struct OutboxMessage {
    std::string message_id;
    std::string kind;              // signal_proposed | trade_proposal | market_outcome
    std::string subject;
    std::string payload;
    std::string signal_proposal_id;
    std::string candidate_id;
    int64_t created_at_ms = 0;
};

// An outbox row still owed to the bus.
struct PendingPublication {
    std::string message_id;
    std::string subject;
    std::string payload;
    int attempts = 0;
    int64_t created_at_ms = 0;
};

// The newest migration (db/migrations) this binary's statements need. On a
// schema altstrategy-db-migrate has not brought this far every archive would
// fail, and nothing would be published while the service looked alive: the
// service refuses to start instead.
inline constexpr std::string_view kRequiredSchemaMigration = "0013";

struct OutboxBacklog {
    int64_t pending = 0;
    int64_t oldest_pending_created_at_ms = 0;   // 0 when nothing is pending
    int64_t dead_lettered = 0;
};

class DecisionStore {
public:
    explicit DecisionStore(altstrategy::persistence::PgPool& pool) : pool_(pool) {}

    // Write path (idempotent).
    // The error says whether retrying can help (a dropped connection).
    Result<void> save_context_snapshot(const DecisionContext& ctx);
    // Logged on failure; the result says whether the ranking exists to be named.
    Result<void> save_ranking(const RankedProposal& r);
    // A candidate's whole decision -- the candidate, its plan variants, its
    // proposals and the publications they owe -- in ONE transaction (BUG-01):
    // all of it is durable or none of it is, and the publisher sends nothing
    // this did not commit. The error says whether retrying can help.
    Result<void> archive_decision(const StrategyCandidate& c, const std::vector<TradePlanVariant>& plans,
                                  const std::vector<TradeSignalProposal>& proposals,
                                  const std::vector<OutboxMessage>& publications);
    // The outcome and, when given, its publication in one transaction.
    bool save_market_outcome(const MarketOutcome& o, const OutboxMessage* publication = nullptr);
    bool save_counterfactual(const CounterfactualOutcome& c);
    bool save_attribution(const OutcomeAttribution& a);
    bool save_execution_feedback(const ExecutionFeedback& f);
    bool save_posterior(const StrategyPosterior& p);
    bool save_drift(const StrategyDrift& d);
    bool save_lifecycle_transition(const LifecycleTransition& t);
    // True when the strategy has not transitioned inside `cooldown_ms`. A
    // lifecycle that can flip every scan is a lifecycle that means nothing.
    bool lifecycle_cooldown_elapsed(const std::string& strategy_id, int64_t now_ms,
                                    int64_t cooldown_ms);
    bool upsert_training_example(const TradeSignalProposal& s, const MarketOutcome& o,
                                 const DecisionContext& ctx, const std::string& label_spec_hash);

    // Load adaptive state at startup / on refresh.
    //
    // `min_evidence_version` / `min_plan_geometry_version` (QUALITY-05): learn
    // only from outcomes of plans at least this geometry version, and load only
    // posteriors estimated from such evidence (learning.min_plan_geometry_version).
    // 1 keeps every outcome, including those of the defective version-1
    // geometry; 2 is the cutover to clean evidence. Nothing is deleted either way.
    PosteriorTable load_posteriors(int min_evidence_version = 1);
    std::map<std::string, Lifecycle> load_lifecycle_overrides();
    std::map<std::string, ParamValues> load_param_sets();
    // Publish a strategy's tunable schema and, when it has no active set yet,
    // seed one from the schema defaults. Without this the calibrator has no
    // baseline to move and the engine has nothing to load.
    bool ensure_strategy_definition(const engine::StrategyMeta& m);
    bool ensure_parameter_schema(const std::string& strategy_id, const std::string& version,
                                 const std::vector<engine::StrategyParam>& schema);
    // Persist a calibrated set as the new ACTIVE one, superseding the previous.
    bool save_parameter_set(const std::string& strategy_id, const std::string& version,
                            const std::map<std::string, double>& values,
                            const std::string& derived_from, int64_t now_ms);
    // Predicted-vs-actual aggregates per strategy, for calibrate_parameters().
    std::map<std::string, engine::CalibrationTargets> load_calibration_targets(
        int64_t from_ms, int min_plan_geometry_version = 1);

    // Scheduler helpers.
    // Candidates whose variants have resolved outcomes but no attribution yet.
    std::vector<AttributionWork> load_unattributed(int limit = 60);
    // Per-strategy baseline-vs-recent metrics for drift detection. `split_ms`
    // divides the two windows; `from_ms` bounds the baseline.
    std::map<std::string, std::pair<std::string, engine::DriftInputs>>
        // No `now_ms`: candidate frequency is normalised by the number of
        // decision contexts each window offered, not by elapsed time, so the
        // wall clock cannot affect the result. It was taken and discarded, which
        // is a parameter promising an influence it does not have.
        load_drift_inputs(int64_t from_ms, int64_t split_ms, int min_plan_geometry_version = 1);
    std::vector<PendingProposal> proposals_awaiting_outcome(int64_t now_ms, int limit = 500);

    // Candidates old enough for every model horizon to have elapsed and with no
    // market outcome yet, newest first (an unresolvable old one can never
    // starve newer ones).
    struct PendingCandidate {
        std::string candidate_id;
        int64_t canonical_pair_id = 0;
        Side side = Side::unspecified;
        int64_t created_at_ms = 0;
        std::optional<double> cost_bps;
    };
    std::vector<PendingCandidate> candidates_awaiting_market_outcome(int64_t now_ms, int limit);
    bool save_candidate_market_outcome(const CandidateMarketOutcome& o);

    // Resolved proposals with the geometry they used, for replaying alternative
    // stop/target grids against the same price path.
    struct ReplayProposal {
        PendingProposal p;
        std::string regime;
        double atr_used = 0;      // price units, recovered from stop distance
    };
    std::vector<ReplayProposal> load_replay_sample(int limit = 400);

    bool save_calibration_run(const std::string& run_id, const std::string& target_kind,
                              const std::string& scope_json, const std::string& result_json);

    // Resolved outcomes joined to the candidate that produced them. Only trades
    // that actually took a position: ENTRY_NOT_REACHED has no return, and
    // counting it as zero would measure the entry geometry, not the edge.
    // `from_ms` is the learning epoch: outcomes produced BEFORE a material
    // change to the engine describe a system that no longer exists, and training
    // on them re-derives the old verdict no matter how often the derived tables
    // are cleared. 0 means "all history".
    std::vector<OutcomeSample> load_outcome_samples(int limit = 20000, int64_t from_ms = 0,
                                                    int min_plan_geometry_version = 1);
    bool mark_proposal_state(const std::string& id, ProposalState state, const std::string& reason,
                             int64_t at_ms);

    // Hypotheses the archive says are still open at `now_ms` (the candidates
    // whose setup time + horizon lies ahead), to restore the de-duplication
    // book after a restart. nullopt when the read failed.
    struct OpenHypothesis {
        int64_t canonical_pair_id = 0;
        std::string strategy_id;
        std::string parameter_set_id;
        Side side = Side::unspecified;
        int64_t setup_time_ms = 0;
        int64_t open_until_ms = 0;
        double ref_price = 0;
        double atr = 0;
    };
    std::optional<std::vector<OpenHypothesis>> load_open_hypotheses(int64_t now_ms);

    // Publication outbox (OutboxPublisher). A failed read is nullopt, never an
    // empty backlog.
    std::optional<std::vector<PendingPublication>> due_publications(int64_t now_ms, int limit);
    bool mark_published(const std::string& message_id, int64_t at_ms, uint64_t stream_sequence);
    bool mark_publish_failed(const std::string& message_id, int attempts, int64_t next_attempt_at_ms,
                             const std::string& error);
    bool mark_dead_lettered(const std::string& message_id, int64_t at_ms, const std::string& error);
    std::optional<OutboxBacklog> outbox_backlog();

    // The newest version in altstrategy-db-migrate's ledger, resolved on this
    // connection exactly as the migrator resolves it (unqualified, same DSN);
    // "" when no ledger is visible. nullopt: the read itself failed.
    std::optional<std::string> newest_schema_migration();

    // One row per table purged by the current altstrategy.retention_policies pass
    // (§25, §39, §52). Bounds on-disk size to a size the operator chose per table
    // instead of unbounded growth; see db/migrations/0001_altstrategy_schema.sql.
    struct RetentionResult {
        std::string table_name;
        int64_t deleted_rows = 0;
        bool ok = true;
        // Why the sweep failed. Without it `retention_table_failed` said only
        // that a table had failed, and two policies naming a column their table
        // does not have repeated that same silent warning every day since
        // deployment with nothing to diagnose from.
        std::string error;
    };
    std::vector<RetentionResult> run_retention(int64_t now_ms);

private:
    altstrategy::persistence::PgPool& pool_;
};

} // namespace altstrategy::engine_io
