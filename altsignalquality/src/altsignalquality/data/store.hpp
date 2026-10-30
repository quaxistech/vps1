#pragma once

// All SQL AltSignalQuality issues, in one place.
//
// Two schemas are touched, with very different rights:
//   * `altsignalquality` — owned; read and written.
//   * `altstrategy`      — read-only, by plain SELECT over the SAME connection
//                          (both schemas live in the shared AltSpotter
//                          database). Nothing here ever writes to it.
//
// Every write is idempotent on a deterministic natural key, so a NATS
// redelivery, a restart mid-batch or a repeated maintenance pass reproduces the
// same rows instead of duplicating them.

#include "altsignalquality/core/config.hpp"
#include "altsignalquality/core/error.hpp"
#include "altsignalquality/core/pg.hpp"
#include "altsignalquality/domain/calibration.hpp"
#include "altsignalquality/domain/feature_contract.hpp"
#include "altsignalquality/domain/priors.hpp"
#include "altsignalquality/domain/signal_snapshot.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace altsignalquality::data {

// ---------------------------------------------------------------------------
// Evaluations
// ---------------------------------------------------------------------------

struct EvaluationRow {
    std::string signal_proposal_id;
    std::string evaluation_id;
    std::string candidate_id;
    int64_t canonical_pair_id = 0;
    std::string symbol;
    std::string strategy_id;
    std::string strategy_version;
    std::string parameter_set_id;
    std::string side;                  // long | short
    std::string proposal_grade;
    bool upstream_recommended = false;
    std::string regime;
    std::string context_id;
    int64_t context_revision = 0;
    int64_t signal_created_at_ms = 0;
    int64_t evaluated_at_ms = 0;
    std::string feature_schema_version;
    std::string feature_schema_hash;
    domain::FeatureVector features;
    bool has_quality_score = false;
    double raw_score = 0;
    double quality_score = 0;
    double decision_threshold = 0;
    std::string decision;              // ACCEPTED | REJECTED
    std::string decision_basis;
    std::vector<std::string> reason_codes;
    std::string model_id;
    std::string calibration_version;
    int64_t inference_latency_us = 0;
    std::string final_signal_id;
    bool published = false;
    // The candidate's original wire bytes; carried only for an accepted signal
    // that has not been published yet.
    std::string candidate_payload;
    std::string publication_payload;
    std::string candidate_sha256;
};

// What an already-stored evaluation looks like when a duplicate arrives.
struct ExistingEvaluation {
    std::string evaluation_id;
    std::string decision;
    std::string final_signal_id;
    bool published = false;
    int64_t evaluated_at_ms = 0;
    std::string candidate_payload;
    std::string publication_payload;
    std::string candidate_sha256;

    ExistingEvaluation() = default;
    explicit ExistingEvaluation(const EvaluationRow& row)
        : evaluation_id(row.evaluation_id), decision(row.decision), final_signal_id(row.final_signal_id),
          published(row.published), evaluated_at_ms(row.evaluated_at_ms),
          candidate_payload(row.candidate_payload), publication_payload(row.publication_payload),
          candidate_sha256(row.candidate_sha256) {}
};

// ---------------------------------------------------------------------------
// Model registry
// ---------------------------------------------------------------------------

struct ActiveModel {
    std::string model_id;
    int32_t model_version = 1;
    std::string artifact_path;
    std::string artifact_checksum;
    std::string feature_schema_version;
    std::string feature_schema_hash;
    int32_t feature_count = 0;
    double decision_threshold = 0.5;
    std::string calibration_version;
    std::string calibration_method;
    std::vector<double> calibration_x;
    std::vector<double> calibration_y;
    int64_t activated_at_ms = 0;
};

struct ModelRegistration {
    std::string model_id;
    std::string algorithm = "lightgbm";
    std::string objective;
    std::string feature_schema_version;
    std::string feature_schema_hash;
    std::string label_spec_version;
    std::string dataset_version;
    std::string hyperparameters_json;
    double decision_threshold = 0.5;
    std::string calibration_version;
    int64_t train_from_ms = 0, train_to_ms = 0;
    int64_t valid_from_ms = 0, valid_to_ms = 0;
    int64_t test_from_ms = 0, test_to_ms = 0;
    int32_t feature_count = 0;
};

struct ArtifactRecord {
    std::string kind;
    std::string path;
    std::string format;
    std::string checksum;
    int64_t size_bytes = 0;
};

struct FoldRecord {
    int32_t fold = 0;
    int64_t train_from_ms = 0, train_to_ms = 0;
    int64_t valid_from_ms = 0, valid_to_ms = 0;
    int64_t test_from_ms = 0, test_to_ms = 0;
    std::string metrics_json;
};

// ---------------------------------------------------------------------------
// Training data
// ---------------------------------------------------------------------------

struct DatasetRow {
    std::string example_id;
    std::string signal_proposal_id;
    std::string strategy_id;
    std::string symbol;
    int64_t canonical_pair_id = 0;
    std::string regime;
    std::string side;
    std::string grade;
    bool upstream_recommended = false;
    int64_t observation_time_ms = 0;
    int64_t label_matured_at_ms = 0;
    std::vector<float> features;
    int label = 0;
    double net_return = 0;
    std::string categorical_outcome;
};

// One AltStrategy proposal whose outcome has matured, as seen from this side.
struct MaturedSignal {
    std::string signal_proposal_id;
    std::string candidate_id;
    int64_t canonical_pair_id = 0;
    std::string symbol;
    std::string strategy_id;
    std::string strategy_version;
    std::string parameter_set_id;
    std::string side;
    std::string proposal_grade;
    bool upstream_recommended = false;
    std::string context_id;
    int64_t observation_time_ms = 0;
    int64_t available_at_ms = 0;
    int64_t label_matured_at_ms = 0;
    std::string categorical_outcome;
    double net_return = 0;
    bool net_return_present = false;
    std::optional<double> mfe;
    std::optional<double> mae;
    std::optional<int64_t> holding_duration_ms;
    std::string altstrategy_example_id;
    std::string regime;
};

// AltStrategy generates three plan geometries per recommended candidate and
// publishes ONLY the balanced one on `signal.proposed.>` — the subject this
// service consumes (engine/io/decision_engine.cpp: `if (is_main) publish(...)`,
// where `is_main` is `profile == balanced` and sets `plan_version = 1`).
//
// The archive, however, holds all three. Training on all of them would fit the
// model to two geometries the live filter never sees: measured on this stack's
// own history, the aggressive and conservative variants are 39 856 of the
// 57 316 labelled rows, and the conservative variant's win rate (0.480) differs
// from the balanced one's (0.354) by more than any effect the filter is trying
// to find. The dataset is therefore restricted to the population the filter is
// actually applied to.
inline constexpr int32_t kLivePlanVersion = 1;

class Store {
public:
    // `max_stop_distance_bps` is a data-sanity bound on the history this
    // service learns from, not a trading rule. AltStrategy's own edge estimator
    // applies the same 1000 bps cut for the same reason: a trade whose stop sat
    // ~90% away from entry is evidence about a defect in a past geometry, not
    // about a strategy's edge, and such rows exist in bulk in this stack's
    // archive. Training on them would teach the filter to recognise a geometry
    // that no longer occurs instead of learning anything about signal quality.
    Store(persistence::PgPool& pool, std::string altstrategy_schema,
          double max_stop_distance_bps = 1000.0)
        : pool_(pool), altstrategy_(std::move(altstrategy_schema)),
          max_stop_distance_bps_(max_stop_distance_bps) {}

    // --- startup -----------------------------------------------------------
    // Publish the compiled-in feature contract and label spec into their
    // registry tables. A version that already exists with a DIFFERENT hash is a
    // hard error: it means the running binary silently redefined a contract
    // that historical rows and shipped models are pinned to.
    [[nodiscard]] Result<void> register_contracts();

    // Confirm the AltStrategy tables this service reads are reachable and have
    // the columns it depends on. Startup fails loudly rather than discovering a
    // missing upstream column inside a training cycle at 03:00.
    [[nodiscard]] Result<void> verify_upstream_readable();

    // --- inference path ----------------------------------------------------
    [[nodiscard]] Result<domain::ContextSnapshot> load_context(const std::string& context_id);

    // The wire proposal carries `context_revision` but not `context_id` — the
    // id lives on the candidate that produced it (altstrategy.strategy_candidates).
    // Resolving it here keeps the lookup one round trip and one place.
    struct ResolvedContext {
        std::string context_id;
        domain::ContextSnapshot snapshot;
    };
    [[nodiscard]] Result<ResolvedContext> load_context_for_candidate(
        const std::string& candidate_id);

    // Insert an evaluation. Returns `nullopt` when the row was created, or the
    // existing row when this signal has already been evaluated — the
    // idempotency claim, taken BEFORE the final signal is published.
    [[nodiscard]] Result<std::optional<ExistingEvaluation>> claim_evaluation(
        const EvaluationRow& row);
    [[nodiscard]] Result<std::optional<ExistingEvaluation>> find_evaluation(
        const std::string& signal_proposal_id);

    [[nodiscard]] Result<void> mark_published(const std::string& signal_proposal_id,
                                              int64_t published_at_ms);

    // Accepted evaluations whose FINAL signal was never confirmed published —
    // the crash-between-persist-and-publish window. Replayed on startup.
    [[nodiscard]] Result<std::vector<EvaluationRow>> load_unpublished(int limit);

    // --- priors ------------------------------------------------------------
    [[nodiscard]] Result<int64_t> load_prior_watermark();
    [[nodiscard]] Result<void> load_prior_cells(domain::PriorAccumulator& acc);
    [[nodiscard]] Result<void> save_prior_cells(const domain::PriorAccumulator& acc,
                                                int64_t watermark_ms);
    // Outcomes that matured in (after_ms, until_ms], oldest first. The upper
    // bound is what makes an as-of replay possible: a backfill batch asks only
    // for what was knowable by the last signal in it.
    [[nodiscard]] Result<std::vector<domain::MaturedOutcome>> load_matured_outcomes(
        int64_t after_ms, int64_t until_ms, int limit);

    // --- dataset -----------------------------------------------------------
    // One matured signal with everything a BACKFILL example needs, assembled by
    // ONE query per batch rather than three per row. The first backfill on a
    // populated box is tens of thousands of signals; at three round trips each
    // that pass takes minutes of pure latency on a loaded host, and it is the
    // pass that has to finish before the service can train at all.
    struct BackfillRow {
        MaturedSignal signal;
        domain::CandidateSignal candidate;
        domain::ContextSnapshot context;
    };
    [[nodiscard]] Result<std::vector<BackfillRow>> load_backfill_rows(
        const std::string& feature_schema_version, const std::string& label_spec_version,
        int64_t not_before_ms, int64_t max_evaluation_lag_ms, int limit);

    // The LIVE counterpart: the matured signal plus the vector this service
    // already stored for it, in one query.
    //
    // `max_evaluation_lag_ms` is what makes a LIVE row point-in-time correct.
    // The stored vector's prior features were read from the aggregate of
    // "everything matured so far" AT EVALUATION TIME — which equals "everything
    // knowable at T0" only when the evaluation happened promptly after T0. A
    // durable consumer replaying a month of history evaluates old signals
    // today, and their priors would then know outcomes from after the signal.
    // Such rows are excluded here and picked up by the BACKFILL path instead,
    // where the priors are genuinely replayed as-of.
    struct LiveRow {
        MaturedSignal signal;
        domain::FeatureVector features;
    };
    [[nodiscard]] Result<std::vector<LiveRow>> load_live_rows(
        const std::string& feature_schema_version, const std::string& label_spec_version,
        int64_t max_evaluation_lag_ms, int limit);

    struct TrainingExampleWrite {
        MaturedSignal signal;
        domain::FeatureVector features;
        int label = 0;
        std::string origin;   // LIVE | BACKFILL
    };
    [[nodiscard]] Result<int64_t> insert_training_examples(
        const std::vector<TrainingExampleWrite>& rows,
        const std::string& feature_schema_version, const std::string& feature_schema_hash,
        const std::string& label_spec_version);

    [[nodiscard]] Result<std::vector<DatasetRow>> load_dataset(
        const std::string& feature_schema_version, const std::string& label_spec_version,
        int64_t from_ms, int64_t max_rows);

    [[nodiscard]] Result<int64_t> count_dataset(const std::string& feature_schema_version,
                                                const std::string& label_spec_version,
                                                int64_t from_ms);

    [[nodiscard]] Result<void> record_dataset_version(const std::string& dataset_version,
                                                      const std::string& feature_schema_version,
                                                      const std::string& label_spec_version,
                                                      int64_t rows, int64_t positives,
                                                      int64_t from_ms, int64_t to_ms,
                                                      const std::string& content_hash);

    // --- model registry ----------------------------------------------------
    [[nodiscard]] Result<std::optional<ActiveModel>> load_active_model(
        const std::string& objective);

    [[nodiscard]] Result<void> register_model(const ModelRegistration& reg,
                                              const std::vector<ArtifactRecord>& artifacts,
                                              const std::vector<FoldRecord>& folds,
                                              const domain::Calibration& calibration,
                                              const std::string& calibration_metrics_json,
                                              const std::vector<std::pair<std::string, double>>&
                                                  scoped_metrics,
                                              const std::string& trained_detail_json);

    [[nodiscard]] Result<void> reject_model(const std::string& model_id,
                                            const std::string& detail_json);
    [[nodiscard]] Result<void> approve_model(const std::string& model_id);
    // Transactional champion switch: lock the candidate and the current ACTIVE
    // row, retire, activate, audit — so two concurrent promotions cannot both
    // believe they won.
    [[nodiscard]] Result<std::string> promote_model(const std::string& model_id,
                                                    const std::string& objective,
                                                    const std::string& checksum);
    [[nodiscard]] Result<bool> has_active_model(const std::string& objective);
    [[nodiscard]] Result<int64_t> last_training_ms(const std::string& objective);
    [[nodiscard]] Result<std::string> model_status_json(const std::string& objective, int limit);
    [[nodiscard]] Result<std::vector<std::pair<std::string, std::string>>> list_model_statuses();

    [[nodiscard]] Result<std::string> begin_training_run(const std::string& trigger);
    [[nodiscard]] Result<void> finish_training_run(const std::string& run_id,
                                                   const std::string& status,
                                                   const std::string& model_id, int64_t rows,
                                                   const std::string& detail_json,
                                                   const std::string& error);

    // --- retention ---------------------------------------------------------
    struct RetentionResult {
        std::string table_name;
        int64_t deleted_rows = 0;
    };
    [[nodiscard]] Result<std::vector<RetentionResult>> run_retention(int64_t now_ms);

    [[nodiscard]] persistence::PgPool& pool() { return pool_; }
    [[nodiscard]] const std::string& altstrategy_schema() const { return altstrategy_; }
    [[nodiscard]] double max_stop_distance_bps() const { return max_stop_distance_bps_; }

private:
    persistence::PgPool& pool_;
    std::string altstrategy_;
    double max_stop_distance_bps_ = 1000.0;
};

// Deterministic ids. Everything this service creates is addressable by its
// natural key, so replaying an event reproduces the same id.
[[nodiscard]] std::string make_evaluation_id(const std::string& signal_proposal_id);
[[nodiscard]] std::string make_final_signal_id(const std::string& signal_proposal_id);
[[nodiscard]] std::string make_example_id(const std::string& signal_proposal_id,
                                          const std::string& feature_schema_version,
                                          const std::string& label_spec_version);

} // namespace altsignalquality::data
