#include "persistence/retention.hpp"

#include "logging/logger.hpp"
#include "core/time.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace altspotter::persistence {

namespace {

// Parent tables whose partitions retention maintains and may drop.
// Raw books/trades are HOURLY: they are the fastest-growing tables and are kept
// for hours, so a daily partition (whose upper bound is in the future all day)
// would leave retention unable to reclaim anything until midnight.
// Everything whose horizon is measured in hours-to-weeks is hourly; only the
// 1m/aggregate series (months) are monthly.
constexpr std::array<const char*, 8> kHourlyParents = {
    "market_trades_raw_core", "market_trades_raw_other",
    "market_orderbook_snapshots_core", "market_orderbook_snapshots_other",
    "market_trade_aggregates_1s_core", "market_trade_aggregates_1s_other",
    "market_book_aggregates_1s_core", "market_book_aggregates_1s_other"};
constexpr std::array<const char*, 6> kMonthlyParents = {
    "market_candles_1m", "market_candles_agg", "market_derivatives_context_1m",
    "market_bbo_1m", "market_trade_aggregates_1m", "market_book_aggregates_1m"};

// Resolve a policy (table_name, retention_class) to the partition parent whose
// children are dropped. LIST-partitioned tables drop under the class sub-parent.
std::string policy_parent(const std::string& table, const std::string& cls) {
    if (cls == "core") return table + "_core";
    if (cls == "other") return table + "_other";
    return table; // 'all'
}

// Rank of a budget state on the escalation ladder. An unknown or empty name is
// NORMAL, so a first measurement applies the plain entry thresholds.
int budget_level(std::string_view state) {
    if (state == "HARD_LIMIT") return 4;
    if (state == "CRITICAL") return 3;
    if (state == "CLEANUP") return 2;
    if (state == "WARNING") return 1;
    return 0;
}

} // namespace

Result<BudgetStatus> measure_budget(PgPool& pool, const RetentionConfig& cfg,
                                    std::string_view previous_state) {
    BudgetStatus st;
    st.hard_limit_bytes = cfg.hard_limit_bytes;
    // pg_database_size is the authoritative measurement and must succeed.
    auto res = pool.with_txn([&](pqxx::work& txn) {
        st.db_bytes = txn.exec1("SELECT pg_database_size(current_database())")[0].as<int64_t>();
    });
    if (!res) return std::unexpected(res.error());

    // WAL size is best-effort and needs pg_monitor; a permission failure here
    // aborts its own transaction only — it must never fail the whole budget
    // measurement (the production role is not a superuser).
    auto wal = pool.with_txn([&](pqxx::work& txn) {
        const auto w = txn.exec1(
            "SELECT COALESCE(sum((pg_stat_file('pg_wal/' || name)).size), 0) "
            "FROM pg_ls_dir('pg_wal') AS name WHERE name ~ '^[0-9A-F]{24}$'");
        st.wal_bytes = w[0].as<int64_t>();
    });
    (void)wal; // ignore: WAL size is optional telemetry

    st.ratio = cfg.hard_limit_bytes > 0
                   ? static_cast<double>(st.db_bytes) / static_cast<double>(cfg.hard_limit_bytes)
                   : 0.0;
    // Escalation ladder with hysteresis. A level is ENTERED on its threshold
    // and only LEFT once the ratio falls `hysteresis_margin` back below it.
    // Retention frees space the moment it escalates, so a bare threshold
    // comparison drops straight back under the line and re-escalates minutes
    // later — the observed FULL -> CLEANUP -> CRITICAL -> FULL cycle. Each lap
    // toggles raw book and trade persistence, so the shed windows are holes in
    // the series the microstructure aggregates are computed from.
    const double margin = std::max(0.0, cfg.hysteresis_margin);
    const int previous = budget_level(previous_state);
    const auto at_least = [&](int level, double entry) {
        return previous >= level ? st.ratio >= entry - margin : st.ratio >= entry;
    };

    if (at_least(4, 1.0)) {
        st.state = "HARD_LIMIT";
        st.mode = PersistenceMode::core_only;
    } else if (at_least(3, cfg.critical_ratio)) {
        st.state = "CRITICAL";
        st.mode = PersistenceMode::raw_trades_disabled; // books already off (see below)
    } else if (at_least(2, cfg.cleanup_ratio)) {
        st.state = "CLEANUP";
        st.mode = PersistenceMode::book_storage_disabled;
    } else if (at_least(1, cfg.warning_ratio)) {
        st.state = "WARNING";
        st.mode = PersistenceMode::full;
    } else {
        st.state = "NORMAL";
        st.mode = PersistenceMode::full;
    }
    return st;
}

Result<int> ensure_partitions(PgPool& pool) {
    int created = 0;
    int deferred = 0;

    // ONE parent per transaction, each with a bounded lock wait.
    //
    // Attaching a partition takes AccessExclusiveLock on the parent while the
    // ingest path holds RowExclusiveLock on it. Doing every parent inside a
    // single transaction meant this DDL kept the locks it had already taken
    // while queueing for the next parent, so a writer moving through the same
    // tables in the opposite order closed a cycle and PostgreSQL killed one
    // side — the observed `runtime_trades_insert` deadlock. A transaction per
    // parent never holds two parent locks at once, so the cycle cannot form.
    //
    // A parent that is busy right now is deferred, not fatal: partitions are
    // pre-created hours ahead of when they are needed and the next pass retries.
    const auto ensure_one = [&](const char* parent, const char* sql) {
        auto res = pool.with_txn([&](pqxx::work& txn) {
            txn.exec("SET LOCAL lock_timeout = '3s'");
            const auto r = txn.exec_params(sql, parent);
            created += r[0][0].as<int>();
        });
        if (!res) {
            ++deferred;
            log::warn("partition_ensure_deferred",
                      {{"parent", std::string(parent)}, {"message", res.error().message}});
        }
    };

    for (const char* p : kHourlyParents) {
        ensure_one(p,
                   "SELECT altspotter_ensure_time_partitions($1::regclass, 'hour', "
                   "now() - interval '6 hours', now() + interval '12 hours')");
    }
    for (const char* p : kMonthlyParents) {
        ensure_one(p,
                   "SELECT altspotter_ensure_time_partitions($1::regclass, 'month', now(), "
                   "now() + interval '3 months')");
    }

    // Every parent yielding is a real problem (the whole database is locked up),
    // not the routine contention this tolerates.
    if (deferred > 0 &&
        deferred == static_cast<int>(kHourlyParents.size() + kMonthlyParents.size())) {
        return std::unexpected(Error{ErrorCode::database_error,
                                     "partition maintenance could not lock any parent table", true});
    }
    return created;
}

Result<DropOutcome> apply_retention(PgPool& pool, const RetentionConfig& cfg, bool aggressive,
                                    bool dry_run) {
    DropOutcome out;
    if (auto b = measure_budget(pool, cfg); b) out.bytes_before = b->db_bytes;

    struct Policy {
        std::string table;
        std::string cls;
        std::string keep_interval; // SQL interval literal to keep
        int priority;
    };
    std::vector<Policy> policies;
    auto load = pool.with_txn([&](pqxx::work& txn) {
        // Routine keeps target; aggressive keeps only the minimum floor.
        const auto rows = txn.exec(
            "SELECT table_name, retention_class, "
            "       (target_keep)::text, (minimum_keep)::text, cleanup_priority "
            "FROM retention_policies ORDER BY cleanup_priority");
        for (const auto& r : rows) {
            Policy p;
            p.table = r[0].as<std::string>();
            p.cls = r[1].as<std::string>();
            p.keep_interval = aggressive ? r[3].as<std::string>() : r[2].as<std::string>();
            p.priority = r[4].as<int>();
            policies.push_back(std::move(p));
        }
    });
    if (!load) return std::unexpected(load.error());

    for (const auto& p : policies) {
        const std::string parent = policy_parent(p.table, p.cls);

        // Read the expiry list in its own short transaction, then drop each
        // partition separately (below). Holding one transaction across many
        // DROPs would keep an AccessExclusiveLock on every dropped partition
        // until commit, which deadlocks against the aggregation queries that
        // read the same parents.
        std::vector<std::string> expired;
        auto listed = pool.with_txn([&](pqxx::work& txn) {
            // Guard: the parent must exist (some class parents only for LIST tables).
            const auto exists = txn.exec_params("SELECT to_regclass($1) IS NOT NULL", parent);
            if (!exists[0][0].as<bool>()) return;
            const auto parts = txn.exec_params(
                "SELECT partition_name, upper_bound FROM altspotter_partition_bounds($1::regclass) "
                "WHERE upper_bound <= now() - $2::interval ORDER BY upper_bound",
                parent, p.keep_interval);
            for (const auto& r : parts) expired.push_back(r[0].as<std::string>());
        });
        if (!listed) {
            log::warn("retention_list_failed",
                      {{"parent", parent}, {"message", listed.error().message}});
            continue;
        }

        for (const auto& name : expired) {
            if (dry_run) {
                out.dropped.push_back(name);
                ++out.partitions_dropped;
                continue;
            }
            auto res = pool.with_txn([&](pqxx::work& txn) {
                // Bounded wait: a partition currently being read is skipped and
                // retried next cycle instead of stalling ingestion or
                // deadlocking with a reader.
                txn.exec("SET LOCAL lock_timeout = '3s'");
                // DROP TABLE detaches + removes the partition; no row scan.
                txn.exec("DROP TABLE IF EXISTS " + txn.quote_name(name));
            });
            if (!res) {
                // Lock timeout / deadlock here is expected contention, not a
                // failure: the partition is still expired and will be retried.
                log::info("retention_drop_deferred",
                          {{"partition", name}, {"message", res.error().message}});
                continue;
            }
            out.dropped.push_back(name);
            ++out.partitions_dropped;
        }
    }

    if (auto b = measure_budget(pool, cfg); b) out.bytes_after = b->db_bytes;
    return out;
}

Result<void> record_statistics(PgPool& pool, const BudgetStatus& budget) {
    return pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO storage_budget_snapshots (db_bytes, hard_limit_bytes, state, "
            "persistence_mode, wal_bytes) VALUES ($1, $2, $3, $4, $5)",
            budget.db_bytes, budget.hard_limit_bytes, budget.state,
            std::string(persistence_mode_label(budget.mode)),
            budget.wal_bytes ? std::optional<int64_t>(*budget.wal_bytes) : std::nullopt);
        // Per-table size stats. pg_table_size on a partitioned parent is 0, so
        // sum the whole partition tree (all descendant partitions) per top-level
        // market table.
        txn.exec(
            "INSERT INTO storage_table_statistics (table_name, table_bytes, index_bytes, "
            "toast_bytes, row_count_estimate) "
            "SELECT c.relname, "
            "  COALESCE((SELECT sum(pg_table_size(t.relid)) "
            "            FROM pg_partition_tree(c.oid) t WHERE t.isleaf), 0), "
            "  COALESCE((SELECT sum(pg_indexes_size(t.relid)) "
            "            FROM pg_partition_tree(c.oid) t WHERE t.isleaf), 0), "
            "  0, "
            "  COALESCE((SELECT sum(l.reltuples)::bigint FROM pg_partition_tree(c.oid) t "
            "            JOIN pg_class l ON l.oid = t.relid WHERE t.isleaf), 0) "
            "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
            "WHERE n.nspname = 'public' AND c.relkind = 'p' "
            "  AND c.relispartition = false AND c.relname LIKE 'market\\_%'");
    });
}



Result<int64_t> prune_operational_tables(PgPool& pool, const OpsRetentionConfig& cfg) {
    struct Rule {
        const char* table;
        const char* time_column;
        int days;
        const char* extra_predicate; // nullptr = none
    };
    const Rule rules[] = {
        {"ingestion_runs", "started_at", cfg.ingestion_runs_days, nullptr},
        {"rejected_market_events", "received_at", cfg.rejected_events_days, nullptr},
        {"retention_runs", "started_at", cfg.retention_runs_days, nullptr},
        {"storage_budget_snapshots", "captured_at", cfg.budget_snapshots_days, nullptr},
        {"storage_table_statistics", "captured_at", cfg.table_statistics_days, nullptr},
        {"global_universe_events", "created_at", cfg.universe_events_days, nullptr},
        {"history_gaps", "resolved_at", cfg.resolved_gaps_days,
         "status IN ('repaired', 'unrepairable') AND resolved_at IS NOT NULL"},
        {"market_funding_settlements", "funding_time", cfg.funding_settlements_days, nullptr},
    };

    int64_t removed = 0;
    for (const auto& rule : rules) {
        if (rule.days <= 0) continue;
        std::string where = std::string(rule.time_column) + " < now() - make_interval(days => " +
                            std::to_string(rule.days) + ")";
        if (rule.extra_predicate != nullptr) {
            where += " AND (" + std::string(rule.extra_predicate) + ")";
        }
        int64_t deleted = 0;
        auto res = pool.with_txn([&](pqxx::work& txn) {
            const auto r = txn.exec("DELETE FROM " + std::string(rule.table) + " WHERE " + where);
            deleted = r.affected_rows();
        });
        if (!res) {
            log::warn("ops_prune_failed",
                      {{"table", rule.table}, {"message", res.error().message}});
            continue;
        }
        if (deleted > 0) {
            log::info("ops_prune", {{"table", rule.table}, {"rows", deleted}});
        }
        removed += deleted;
    }
    return removed;
}

} // namespace altspotter::persistence
