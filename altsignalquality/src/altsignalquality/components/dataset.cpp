#include "altsignalquality/components/dataset.hpp"

#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/pg.hpp"
#include "altsignalquality/core/readiness.hpp"
#include "altsignalquality/core/shutdown.hpp"
#include "altsignalquality/core/time.hpp"
#include "altsignalquality/data/store.hpp"
#include "altsignalquality/domain/feature_contract.hpp"
#include "altsignalquality/domain/labeling.hpp"
#include "altsignalquality/domain/priors.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace altsignalquality {

namespace {

struct DatasetConfig {
    int64_t postgres_pool_size = 2;
    int64_t prior_interval_sec = 300;
    int64_t example_interval_sec = 300;
    int64_t retention_interval_sec = 21600;   // 6h
    int64_t prior_batch = 20000;
    int64_t example_batch = 2000;
    int64_t backfill_batch = 1000;
    bool backfill_enabled = true;
    int64_t backfill_lookback_days = 400;
    // How many batches one pass may run. The first backfill on a populated box
    // is tens of thousands of signals; draining it a batch every five minutes
    // would take a day before the service could train at all.
    int64_t backfill_max_batches_per_pass = 60;
    // Mirrors evaluator.max_signal_age_sec: an evaluation that lagged its
    // signal by more than this had priors that were not as-of, so its stored
    // vector is not a point-in-time record and the row is rebuilt by the
    // backfill path instead. Kept in this section so one number governs both
    // the live freshness gate and the dataset's definition of a LIVE row.
    int64_t max_evaluation_lag_sec = 900;
    double prior_strength = 20.0;
    double prior_half_life_days = 30.0;
};

DatasetConfig load_dataset_config(const YAML::Node& root) {
    DatasetConfig c;
    const YAML::Node n = root["dataset"];
    c.postgres_pool_size = config::opt_int(n, "postgres_pool_size", c.postgres_pool_size);
    c.prior_interval_sec = config::opt_int(n, "prior_interval_sec", c.prior_interval_sec);
    c.example_interval_sec = config::opt_int(n, "example_interval_sec", c.example_interval_sec);
    c.retention_interval_sec =
        config::opt_int(n, "retention_interval_sec", c.retention_interval_sec);
    c.prior_batch = config::opt_int(n, "prior_batch", c.prior_batch);
    c.example_batch = config::opt_int(n, "example_batch", c.example_batch);
    c.backfill_batch = config::opt_int(n, "backfill_batch", c.backfill_batch);
    c.backfill_enabled = config::opt_bool(n, "backfill_enabled", c.backfill_enabled);
    c.backfill_lookback_days =
        config::opt_int(n, "backfill_lookback_days", c.backfill_lookback_days);
    c.backfill_max_batches_per_pass =
        config::opt_int(n, "backfill_max_batches_per_pass", c.backfill_max_batches_per_pass);
    c.max_evaluation_lag_sec =
        config::opt_int(root["evaluator"], "max_signal_age_sec", c.max_evaluation_lag_sec);
    const YAML::Node p = root["priors"];
    c.prior_strength = config::opt_double(p, "prior_strength", c.prior_strength);
    c.prior_half_life_days = config::opt_double(p, "half_life_days", c.prior_half_life_days);
    return c;
}

domain::PriorConfig prior_config(const DatasetConfig& c) {
    domain::PriorConfig p;
    p.prior_strength = c.prior_strength;
    p.half_life_days = c.prior_half_life_days;
    return p;
}

// --- the three jobs ---------------------------------------------------------

// Fold every outcome matured since the watermark into the persisted aggregates.
// Incremental and restart-safe: the watermark only advances after the cells for
// that batch are committed in the same transaction.
Result<int64_t> refresh_priors(data::Store& store, const DatasetConfig& cfg) {
    auto watermark = store.load_prior_watermark();
    if (!watermark) return std::unexpected(watermark.error());

    domain::PriorAccumulator acc(prior_config(cfg));
    if (auto r = store.load_prior_cells(acc); !r) return std::unexpected(r.error());

    auto outcomes = store.load_matured_outcomes(*watermark, timeutil::now_ms(),
                                                static_cast<int>(cfg.prior_batch));
    if (!outcomes) return std::unexpected(outcomes.error());
    if (outcomes->empty()) return int64_t{0};

    int64_t new_watermark = *watermark;
    for (const auto& o : *outcomes) {
        acc.apply(o);
        new_watermark = std::max(new_watermark, o.matured_at_ms);
    }
    if (auto r = store.save_prior_cells(acc, new_watermark); !r) return std::unexpected(r.error());
    return static_cast<int64_t>(outcomes->size());
}

struct MaterializeResult {
    int64_t live = 0;
    int64_t backfilled = 0;
    // Rows AltStrategy resolved into an outcome this label spec cannot use
    // (see domain/labeling.hpp). Expected and normal, not an error — but worth
    // reporting, because a sudden jump means the upstream outcome mix changed.
    int64_t skipped_unlabelable = 0;
};

// LIVE rows: the signal was scored by this service at T0, so the stored vector
// IS the training vector. Nothing is reconstructed, so nothing can drift.
Result<int64_t> materialize_live(data::Store& store, const DatasetConfig& cfg,
                                 MaterializeResult& stats) {
    const std::string fsv(domain::kFeatureSchemaVersion);
    const std::string lsv(domain::kLabelSpecVersion);
    auto rows = store.load_live_rows(fsv, lsv, cfg.max_evaluation_lag_sec * 1000,
                                     static_cast<int>(cfg.example_batch));
    if (!rows) return std::unexpected(rows.error());
    if (rows->empty()) return int64_t{0};

    std::vector<data::Store::TrainingExampleWrite> writes;
    writes.reserve(rows->size());
    for (const auto& row : *rows) {
        domain::OutcomeRecord rec;
        rec.categorical = row.signal.categorical_outcome;
        rec.net_return_after_costs = row.signal.net_return;
        rec.net_return_present = row.signal.net_return_present;
        const auto label = domain::label_of(rec);
        if (!label.usable) {
            ++stats.skipped_unlabelable;
            continue;
        }
        writes.push_back({row.signal, row.features, label.label, "LIVE"});
    }
    auto inserted = store.insert_training_examples(writes, fsv, domain::feature_schema_hash(), lsv);
    if (!inserted) return std::unexpected(inserted.error());
    stats.live += *inserted;
    return *inserted;
}

// BACKFILL rows: signals this service never saw (it was not running, or it ran
// under an older feature schema). Reconstructed through the SAME assembler from
// AltStrategy's point-in-time archive, with the historical priors replayed
// AS OF each signal rather than read from today's aggregates — reading today's
// would hand a signal from June knowledge of August outcomes.
Result<int64_t> materialize_backfill(data::Store& store, const DatasetConfig& cfg,
                                     MaterializeResult& stats) {
    const std::string fsv(domain::kFeatureSchemaVersion);
    const std::string lsv(domain::kLabelSpecVersion);
    const int64_t not_before =
        timeutil::now_ms() - cfg.backfill_lookback_days * timeutil::kMsPerDay;

    // The maturation history is loaded ONCE per pass, not once per batch. The
    // as-of sweep ignores anything that matured after a given signal, so a
    // single "everything matured so far" read is correct for every batch — and
    // the first backfill on a populated box is dozens of batches, each of which
    // would otherwise re-read tens of thousands of outcomes.
    std::optional<std::vector<domain::MaturedOutcome>> history;
    int64_t total_inserted = 0;

    for (int64_t batch = 0; batch < cfg.backfill_max_batches_per_pass; ++batch) {
        if (ShutdownToken::instance().stop_requested()) break;
        auto rows = store.load_backfill_rows(fsv, lsv, not_before,
                                             cfg.max_evaluation_lag_sec * 1000,
                                             static_cast<int>(cfg.backfill_batch));
        if (!rows) return std::unexpected(rows.error());
        if (rows->empty()) break;

        if (!history) {
            auto loaded = store.load_matured_outcomes(0, timeutil::now_ms(), 1'000'000);
            if (!loaded) return std::unexpected(loaded.error());
            history = std::move(*loaded);
            log::info("backfill_history_loaded",
                      {{"matured_outcomes", static_cast<int64_t>(history->size())}});
        }

        std::vector<domain::PriorReadRequest> reads;
        reads.reserve(rows->size());
        for (const auto& row : *rows) {
            domain::PriorReadRequest r;
            r.observation_time_ms = row.signal.observation_time_ms;
            r.strategy_id = row.signal.strategy_id;
            r.canonical_pair_id = row.signal.canonical_pair_id;
            r.regime = row.signal.regime;
            r.grade = row.signal.proposal_grade;
            r.side = row.signal.side == "short" ? 2 : 1;
            reads.push_back(std::move(r));
        }
        const auto priors = domain::sweep_as_of(*history, reads, prior_config(cfg));

        std::vector<data::Store::TrainingExampleWrite> writes;
        writes.reserve(rows->size());
        for (size_t i = 0; i < rows->size(); ++i) {
            const auto& row = (*rows)[i];
            domain::OutcomeRecord rec;
            rec.categorical = row.signal.categorical_outcome;
            rec.net_return_after_costs = row.signal.net_return;
            rec.net_return_present = row.signal.net_return_present;
            const auto label = domain::label_of(rec);
            if (!label.usable) {
                ++stats.skipped_unlabelable;
                continue;
            }
            domain::SignalSnapshot snapshot;
            snapshot.candidate = row.candidate;
            snapshot.context = row.context;
            snapshot.priors = priors[i];
            writes.push_back({row.signal, domain::assemble(snapshot), label.label, "BACKFILL"});
        }
        auto inserted =
            store.insert_training_examples(writes, fsv, domain::feature_schema_hash(), lsv);
        if (!inserted) return std::unexpected(inserted.error());
        stats.backfilled += *inserted;
        total_inserted += *inserted;
        // Nothing new was written although rows came back: every row in this
        // batch was unlabelable. Another identical batch would return the same
        // rows forever, so stop rather than spin.
        if (*inserted == 0) break;
    }
    return total_inserted;
}

Result<MaterializeResult> materialize_examples(data::Store& store, const DatasetConfig& cfg) {
    MaterializeResult stats;
    if (auto r = materialize_live(store, cfg, stats); !r) return std::unexpected(r.error());
    if (cfg.backfill_enabled) {
        if (auto r = materialize_backfill(store, cfg, stats); !r) return std::unexpected(r.error());
    }
    return stats;
}

} // namespace

struct DatasetComponent::Impl {
    DatasetConfig cfg;
    std::optional<persistence::PgPool> pool;
    std::optional<data::Store> store;
    int64_t next_priors_ms = 0;
    int64_t next_examples_ms = 0;
    int64_t next_retention_ms = 0;
};

DatasetComponent::DatasetComponent(const config::RuntimeConfig& rt)
    : rt_(rt), impl_(std::make_unique<Impl>()) {}

DatasetComponent::~DatasetComponent() = default;

Result<void> DatasetComponent::init() {
    impl_->cfg = load_dataset_config(rt_.root);
    if (impl_->cfg.prior_interval_sec < 10 || impl_->cfg.example_interval_sec < 10 ||
        impl_->cfg.retention_interval_sec < 60 || impl_->cfg.prior_batch < 1 ||
        impl_->cfg.example_batch < 1 || impl_->cfg.backfill_batch < 1 ||
        impl_->cfg.backfill_max_batches_per_pass < 1 ||
        impl_->cfg.max_evaluation_lag_sec < 1 ||
        impl_->cfg.prior_strength <= 0.0 || impl_->cfg.prior_half_life_days <= 0.0) {
        return fail(ErrorCode::config_error, "invalid dataset/priors configuration bounds");
    }
    impl_->pool.emplace(rt_.common.postgres_dsn,
                        static_cast<size_t>(impl_->cfg.postgres_pool_size));
    if (auto probe = impl_->pool->with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); });
        !probe) {
        return fail(ErrorCode::database_error, "postgres unavailable: " + probe.error().message);
    }
    impl_->store.emplace(*impl_->pool, rt_.wiring.altstrategy_schema,
                         rt_.wiring.max_stop_distance_bps);
    if (auto r = impl_->store->verify_upstream_readable(); !r) return r;
    set_component_status(name(), "idle");
    return {};
}

void DatasetComponent::run() {
    auto& shutdown = ShutdownToken::instance();
    auto& cfg = impl_->cfg;
    auto& store = *impl_->store;
    while (!shutdown.stop_requested()) {
        const int64_t now = timeutil::now_ms();
        if (now >= impl_->next_priors_ms) {
            impl_->next_priors_ms = now + cfg.prior_interval_sec * 1000;
            auto folded = refresh_priors(store, cfg);
            if (folded) {
                if (*folded > 0) {
                    log::info("outcome_priors_refreshed", {{"outcomes_folded", *folded}});
                }
            } else {
                log::warn("outcome_prior_refresh_failed", {{"message", folded.error().message}});
            }
        }
        if (!shutdown.stop_requested() && now >= impl_->next_examples_ms) {
            impl_->next_examples_ms = now + cfg.example_interval_sec * 1000;
            auto stats = materialize_examples(store, cfg);
            if (stats) {
                if (stats->live > 0 || stats->backfilled > 0) {
                    log::info("training_examples_materialized",
                              {{"live", stats->live},
                               {"backfilled", stats->backfilled},
                               {"skipped_unlabelable", stats->skipped_unlabelable}});
                }
                set_component_status(name(), "examples+" + std::to_string(stats->live +
                                                                         stats->backfilled));
            } else {
                log::warn("training_example_materialization_failed",
                          {{"message", stats.error().message}});
            }
        }
        if (!shutdown.stop_requested() && now >= impl_->next_retention_ms) {
            impl_->next_retention_ms = now + cfg.retention_interval_sec * 1000;
            auto retention = store.run_retention(now);
            if (retention) {
                int64_t deleted = 0;
                for (const auto& r : *retention) deleted += r.deleted_rows;
                if (deleted > 0) log::info("retention_pass", {{"rows_deleted", deleted}});
            } else {
                log::warn("retention_failed", {{"message", retention.error().message}});
            }
        }
        if (shutdown.wait_for(std::chrono::seconds(10))) break;
    }
}

int run_dataset_once(const config::RuntimeConfig& rt) {
    const auto cfg = load_dataset_config(rt.root);
    persistence::PgPool pool(rt.common.postgres_dsn, 2);
    data::Store store(pool, rt.wiring.altstrategy_schema, rt.wiring.max_stop_distance_bps);
    if (auto r = store.register_contracts(); !r) {
        std::fprintf(stderr, "contract registration failed: %s\n", r.error().message.c_str());
        return 1;
    }
    auto folded = refresh_priors(store, cfg);
    if (!folded) {
        std::fprintf(stderr, "prior refresh failed: %s\n", folded.error().message.c_str());
        return 1;
    }
    auto stats = materialize_examples(store, cfg);
    if (!stats) {
        std::fprintf(stderr, "materialization failed: %s\n", stats.error().message.c_str());
        return 1;
    }
    auto total = store.count_dataset(std::string(domain::kFeatureSchemaVersion),
                                     std::string(domain::kLabelSpecVersion), 0);
    if (!total) {
        std::fprintf(stderr, "dataset count failed: %s\n", total.error().message.c_str());
        return 1;
    }
    std::printf("{\"event\":\"dataset_built\",\"outcomes_folded\":%lld,\"live\":%lld,"
                "\"backfilled\":%lld,\"skipped_unlabelable\":%lld,\"eligible_rows\":%lld}\n",
                static_cast<long long>(*folded), static_cast<long long>(stats->live),
                static_cast<long long>(stats->backfilled),
                static_cast<long long>(stats->skipped_unlabelable),
                static_cast<long long>(*total));
    return 0;
}

} // namespace altsignalquality
