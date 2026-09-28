#include "altml/components/retention_logic.hpp"
#include "altml/components/storage_guard.hpp"

#include "altml/core/logger.hpp"
#include "altml/core/pg.hpp"
#include "altml/core/readiness.hpp"
#include "altml/core/shutdown.hpp"
#include "altml/core/time.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace altml {

namespace {
namespace fs = std::filesystem;

struct StorageConfig {
    int64_t pg_soft_budget_gb = 2;
    int64_t pg_hard_limit_gb = 3;
    int64_t check_interval_sec = 300;
    int64_t retired_model_keep_count = 5;
};

Result<StorageConfig> load_storage_config(const YAML::Node& root) {
    const YAML::Node n = root["storage"];
    StorageConfig c;
    c.pg_soft_budget_gb = config::opt_int(n, "pg_soft_budget_gb", c.pg_soft_budget_gb);
    c.pg_hard_limit_gb = config::opt_int(n, "pg_hard_limit_gb", c.pg_hard_limit_gb);
    c.check_interval_sec = config::opt_int(n, "check_interval_sec", c.check_interval_sec);
    c.retired_model_keep_count =
        config::opt_int(n, "retired_model_keep_count", c.retired_model_keep_count);
    if (c.pg_soft_budget_gb <= 0 || c.pg_hard_limit_gb <= 0 ||
        c.pg_soft_budget_gb > c.pg_hard_limit_gb || c.check_interval_sec < 30 ||
        c.retired_model_keep_count < 1) {
        return fail(ErrorCode::config_error, "invalid storage configuration bounds");
    }
    return c;
}

// One partitioned table this database owns, and how it retains data.
struct RetentionPolicy {
    std::string table_name;   // e.g. "altml.labels"
    int64_t target_keep_days = 0;
    int64_t minimum_keep_days = 0;
};

Result<std::vector<RetentionPolicy>> load_policies(persistence::PgPool& pool) {
    std::vector<RetentionPolicy> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        for (const auto& row : txn.exec(
                 "SELECT table_name, extract(day FROM target_keep)::bigint, "
                 "extract(day FROM minimum_keep)::bigint FROM altml.retention_policies "
                 "WHERE table_name IN ('altml.labels', 'altml.feature_vectors', "
                 "                     'altml.model_predictions') "
                 "ORDER BY cleanup_priority")) {
            out.push_back({row[0].as<std::string>(), row[1].as<int64_t>(), row[2].as<int64_t>()});
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

// Drops whole partitions of `table_name` whose upper bound is older than
// `keep_days`. Never touches a partial/current partition. Returns the number
// of partitions dropped.
Result<int> drop_old_partitions(persistence::PgPool& pool, const std::string& table_name,
                                int64_t keep_days) {
    int dropped = 0;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        for (const auto& row : txn.exec_params(
                 "SELECT partition_name FROM altml.partition_bounds($1::regclass) "
                 "WHERE upper_bound <= now() - make_interval(days => $2::int)",
                 table_name, keep_days)) {
            const std::string part = row[0].as<std::string>();
            txn.exec("DROP TABLE IF EXISTS " + part);
            ++dropped;
        }
    });
    if (!res) return std::unexpected(res.error());
    return dropped;
}

// Deletes retired/rejected/rolled-back models beyond the most recent
// `keep_count`, their DB rows and their on-disk artifact directories. ACTIVE,
// APPROVED and CANDIDATE models are never touched here.
Result<int> prune_old_models(persistence::PgPool& pool, const fs::path& model_root,
                             int64_t keep_count) {
    std::vector<std::string> stale_ids;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        for (const auto& row : txn.exec_params(
                 "SELECT model_id FROM altml.models "
                 "WHERE status IN ('REJECTED','RETIRED','ROLLBACK','FAILED') "
                 "ORDER BY created_at DESC OFFSET $1",
                 keep_count)) {
            stale_ids.push_back(row[0].as<std::string>());
        }
    });
    if (!res) return std::unexpected(res.error());
    for (const auto& id : stale_ids) {
        auto del = pool.with_txn([&](pqxx::work& txn) {
            txn.exec_params("DELETE FROM altml.model_metrics WHERE model_id = $1", id);
            txn.exec_params("DELETE FROM altml.model_walk_forward_runs WHERE model_id = $1", id);
            txn.exec_params("DELETE FROM altml.model_feature_compatibility WHERE model_id = $1",
                            id);
            txn.exec_params("DELETE FROM altml.model_events WHERE model_id = $1", id);
            txn.exec_params("DELETE FROM altml.model_artifacts WHERE model_id = $1", id);
            txn.exec_params("DELETE FROM altml.models WHERE model_id = $1", id);
        });
        if (!del) {
            log::error("model_prune_db_failed", {{"model_id", id}, {"message", del.error().message}});
            continue;
        }
        std::error_code fs_error;
        const fs::path dir = fs::weakly_canonical(model_root / id, fs_error);
        const fs::path root = fs::weakly_canonical(model_root, fs_error);
        if (!fs_error && dir.string().starts_with(root.string())) {
            fs::remove_all(dir, fs_error); // best-effort; DB row is already gone
        }
    }
    return static_cast<int>(stale_ids.size());
}

std::string budget_state(int64_t db_bytes, int64_t soft_bytes, int64_t hard_bytes) {
    if (db_bytes >= hard_bytes) return "HARD_LIMIT";
    if (db_bytes >= soft_bytes) return "CRITICAL";
    if (db_bytes * 100 >= soft_bytes * 70) return "WARNING";
    return "NORMAL";
}

} // namespace

struct StorageGuardComponent::Impl {
    StorageConfig cfg;
    std::optional<persistence::PgPool> pool;
    std::string model_root;
};

StorageGuardComponent::StorageGuardComponent(const config::RuntimeConfig& rt)
    : rt_(rt), impl_(std::make_unique<Impl>()) {}

StorageGuardComponent::~StorageGuardComponent() = default;

Result<void> StorageGuardComponent::init() {
    auto cfg = load_storage_config(rt_.root);
    if (!cfg) return std::unexpected(cfg.error());
    impl_->cfg = *cfg;
    impl_->model_root = rt_.project.model_root;
    impl_->pool.emplace(rt_.common.postgres_dsn, 1);
    if (auto probe = impl_->pool->with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); });
        !probe) {
        return fail(ErrorCode::database_error, "postgres unavailable: " + probe.error().message);
    }
    set_component_status(name(), "watching database size budget");
    return {};
}

void StorageGuardComponent::run() {
    auto& shutdown = ShutdownToken::instance();
    auto& pool = *impl_->pool;
    const auto& cfg = impl_->cfg;
    const int64_t soft_bytes = cfg.pg_soft_budget_gb * 1024LL * 1024 * 1024;
    const int64_t hard_bytes = cfg.pg_hard_limit_gb * 1024LL * 1024 * 1024;

    while (!shutdown.stop_requested()) {
        const int64_t started = timeutil::now_ms();
        int64_t db_bytes = 0;
        auto size_res = pool.with_txn([&](pqxx::work& txn) {
            db_bytes = txn.exec1("SELECT pg_database_size(current_database())")[0].as<int64_t>();
        });
        if (!size_res) {
            log::error("storage_size_check_failed", {{"message", size_res.error().message}});
            if (shutdown.wait_for(std::chrono::seconds(cfg.check_interval_sec))) break;
            continue;
        }

        const std::string state = budget_state(db_bytes, soft_bytes, hard_bytes);
        set_component_status(name(), "db=" + std::to_string(db_bytes / (1024 * 1024)) +
                                         "MiB state=" + state);
        auto snap = pool.with_txn([&](pqxx::work& txn) {
            txn.exec_params(
                "INSERT INTO altml.storage_snapshots (db_bytes, soft_budget_bytes, "
                "hard_limit_bytes, state) VALUES ($1, $2, $3, $4)",
                db_bytes, soft_bytes, hard_bytes, state);
        });
        if (!snap) log::warn("storage_snapshot_write_failed", {{"message", snap.error().message}});
        if (state == "WARNING") {
            log::warn("storage_budget_warning", {{"db_bytes", db_bytes}, {"soft_budget_bytes", soft_bytes}});
        } else if (state == "CRITICAL" || state == "HARD_LIMIT") {
            log::error("storage_budget_exceeded", {{"db_bytes", db_bytes}, {"state", state}});
        }

        // Routine retention: drop partitions past their target age, always.
        auto policies = load_policies(pool);
        int partitions_dropped = 0;
        if (policies) {
            for (const auto& p : *policies) {
                auto dropped = drop_old_partitions(pool, p.table_name, p.target_keep_days);
                if (dropped) {
                    partitions_dropped += *dropped;
                } else {
                    log::error("retention_drop_failed",
                               {{"table", p.table_name}, {"message", dropped.error().message}});
                }
            }
        } else {
            log::error("retention_policy_load_failed", {{"message", policies.error().message}});
        }
        auto pruned_audit = pool.with_txn([&](pqxx::work& txn) {
            txn.exec("DELETE FROM altml.label_computation_runs "
                     "WHERE started_at < now() - interval '30 days'");
            txn.exec("DELETE FROM altml.model_events "
                     "WHERE created_at < now() - interval '180 days'");
        });
        if (!pruned_audit) {
            log::error("audit_prune_failed", {{"message", pruned_audit.error().message}});
        }

        // Emergency pass: still over the hard limit after routine retention ->
        // cut every policy down to its floor, then cap old model artifacts.
        if (state == "HARD_LIMIT" && policies) {
            for (const auto& p : *policies) {
                auto dropped = drop_old_partitions(pool, p.table_name, p.minimum_keep_days);
                if (dropped) partitions_dropped += *dropped;
            }
            auto pruned = prune_old_models(pool, impl_->model_root, cfg.retired_model_keep_count);
            if (!pruned) {
                log::error("model_prune_failed", {{"message", pruned.error().message}});
            } else if (*pruned > 0) {
                log::warn("models_pruned_under_pressure", {{"count", *pruned}});
            }
        }

        if (partitions_dropped > 0) {
            log::info("retention_partitions_dropped", {{"count", partitions_dropped}});
        }

        // What the passes above actually reclaimed. Measured once and reused for
        // both the deadlock test and the run record, so the decision and the
        // audit row can never disagree.
        int64_t db_bytes_after = db_bytes;
        if (auto sized = pool.with_txn([&](pqxx::work& txn) {
                db_bytes_after =
                    txn.exec1("SELECT pg_database_size(current_database())")[0].as<int64_t>();
            });
            !sized) {
            log::warn("storage_size_recheck_failed", {{"message", sized.error().message}});
        }
        const int64_t bytes_freed = db_bytes - db_bytes_after;

        // Over the limit, nothing freed, and every policy floor still in the
        // way: no future cycle can do better. Say so once per cycle with the
        // numbers that prove it, instead of repeating a generic budget line
        // forever while the trainer starves behind it.
        if (db_bytes_after > hard_bytes && policies) {
            std::vector<storage::PolicyPressure> pressure;
            auto measured = pool.with_txn([&](pqxx::work& txn) {
                for (const auto& p : *policies) {
                    // Only partitions that hold data. An empty partition is not
                    // reclaimable history, and counting one made the guard
                    // believe it had 145 days to work with while every byte it
                    // held sat inside the floor.
                    const auto rows = txn.exec_params(
                        "SELECT coalesce(max(extract(day FROM now() - pb.lower_bound))::bigint, -1) "
                        "FROM altml.partition_bounds($1::regclass) pb "
                        "WHERE pg_total_relation_size(pb.partition_name::regclass) > 1048576",
                        p.table_name);
                    storage::PolicyPressure pp;
                    pp.table_name = p.table_name;
                    pp.minimum_keep_days = p.minimum_keep_days;
                    pp.oldest_partition_age_days = rows[0][0].as<int64_t>();
                    pressure.push_back(std::move(pp));
                }
            });
            if (!measured) {
                log::warn("retention_pressure_probe_failed",
                          {{"message", measured.error().message}});
            } else if (storage::is_retention_deadlock(true, bytes_freed, pressure)) {
                for (const auto& pp : pressure) {
                    log::critical(
                        "retention_deadlock",
                        {{"table", pp.table_name},
                         {"db_bytes", db_bytes},
                         {"hard_limit_bytes", hard_bytes},
                         {"oldest_data_age_days", pp.oldest_partition_age_days},
                         {"minimum_keep_days", pp.minimum_keep_days},
                         {"bytes_freed_this_cycle", bytes_freed},
                         {"partitions_dropped", partitions_dropped},
                         {"message",
                          "retention floor protects all held partitions; the size budget "
                          "and the retention policy cannot both be satisfied"}});
                }
            }
        }
        auto run_record = pool.with_txn([&](pqxx::work& txn) {
            txn.exec_params(
                "INSERT INTO altml.retention_runs (started_at, finished_at, mode, "
                "db_bytes_before, db_bytes_after, partitions_dropped) "
                "VALUES (to_timestamp($1::bigint / 1000.0), now(), 'scheduled', $2, $4, $3)",
                started, db_bytes, partitions_dropped, db_bytes_after);
        });
        if (!run_record) {
            log::warn("retention_run_record_failed", {{"message", run_record.error().message}});
        }

        if (shutdown.wait_for(std::chrono::seconds(cfg.check_interval_sec))) break;
    }
}

} // namespace altml
