// retention_manager: enforces the configured hard database size limit.
//
//   * measures pg_database_size vs the configured hard limit each cycle;
//   * classifies NORMAL/WARNING/CLEANUP/CRITICAL/HARD_LIMIT and switches the
//     process-wide PersistenceGate so raw books, then raw trades, are shed
//     before candles/context (live NATS publishing always continues);
//   * pre-creates upcoming partitions (advisory-locked, no long DDL);
//   * drops whole expired partitions per retention_policies — routine at
//     target_keep, aggressive down to the protected minimum_keep under
//     pressure, lowest cleanup_priority first (never row-by-row DELETE);
//   * records retention_runs / storage_budget_snapshots / storage_table_statistics;
//   * publishes evt.storage.{state,cleanup,critical}.

#include "components/components.hpp"
#include "core/persistence_gate.hpp"
#include "core/readiness.hpp"
#include "core/shutdown.hpp"
#include "config/config.hpp"
#include "logging/logger.hpp"
#include "messaging/codec.hpp"
#include "messaging/nats_client.hpp"
#include "messaging/subjects.hpp"
#include "persistence/pg.hpp"
#include "persistence/retention.hpp"
#include "persistence/txn_log.hpp"
#include "core/time.hpp"

#include <memory>

using namespace altspotter;

namespace {

persistence::RetentionConfig parse_retention_cfg(const config::ServiceContext& ctx) {
    persistence::RetentionConfig c;
    const auto s = ctx.root["storage"];
    if (s && s.IsMap()) {
        c.hard_limit_bytes = config::opt_int(s, "hard_database_limit_bytes", c.hard_limit_bytes);
        c.warning_ratio = config::opt_double(s, "warning_ratio", c.warning_ratio);
        c.cleanup_ratio = config::opt_double(s, "cleanup_ratio", c.cleanup_ratio);
        c.critical_ratio = config::opt_double(s, "critical_ratio", c.critical_ratio);
        c.hysteresis_margin = config::opt_double(s, "hysteresis_margin", c.hysteresis_margin);
    }
    return c;
}

persistence::OpsRetentionConfig parse_ops_cfg(const config::ServiceContext& ctx) {
    persistence::OpsRetentionConfig c;
    const auto o = ctx.root["ops_retention"];
    if (o && o.IsMap()) {
        const auto g = [&](const char* k, int d) {
            return static_cast<int>(config::opt_int(o, k, d));
        };
        c.ingestion_runs_days = g("ingestion_runs_days", c.ingestion_runs_days);
        c.rejected_events_days = g("rejected_events_days", c.rejected_events_days);
        c.retention_runs_days = g("retention_runs_days", c.retention_runs_days);
        c.budget_snapshots_days = g("budget_snapshots_days", c.budget_snapshots_days);
        c.table_statistics_days = g("table_statistics_days", c.table_statistics_days);
        c.universe_events_days = g("universe_events_days", c.universe_events_days);
        c.resolved_gaps_days = g("resolved_gaps_days", c.resolved_gaps_days);
        c.funding_settlements_days = g("funding_settlements_days", c.funding_settlements_days);
    }
    return c;
}

// Reconcile retention_policies from the `retention:` config block, so the
// configured horizons are the single source of truth and can be tuned without a
// migration. Keys map to (table_name, retention_class) rows; `*_min_*` values
// are the protected floors aggressive cleanup may never drop below.
void reconcile_policies(persistence::PgPool& pool, const config::ServiceContext& ctx) {
    const auto r = ctx.root["retention"];
    if (!r || !r.IsMap()) return;

    struct PolicyKey {
        const char* table;
        const char* cls;
        const char* target_key;
        const char* minimum_key;
        int64_t target_default;
        int64_t minimum_default;
        const char* unit; // 'days' | 'hours'
    };
    static constexpr PolicyKey kKeys[] = {
        // Short-lived classes are configured in HOURS: their partitions are
        // hourly, so hours are the unit retention can actually act on.
        {"market_orderbook_snapshots", "other", "orderbook_other_hours",
         "orderbook_other_min_hours", 72, 2, "hours"},
        {"market_orderbook_snapshots", "core", "orderbook_core_hours",
         "orderbook_core_min_hours", 168, 12, "hours"},
        {"market_trades_raw", "other", "raw_trades_other_hours",
         "raw_trades_other_min_hours", 72, 6, "hours"},
        {"market_trades_raw", "core", "raw_trades_core_hours",
         "raw_trades_core_min_hours", 168, 24, "hours"},
        {"market_book_aggregates_1s", "other", "book_1s_other_hours",
         "book_1s_other_min_hours", 168, 72, "hours"},
        {"market_trade_aggregates_1s", "other", "trade_1s_other_hours",
         "trade_1s_other_min_hours", 336, 72, "hours"},
        {"market_book_aggregates_1s", "core", "book_1s_core_hours",
         "book_1s_core_min_hours", 336, 168, "hours"},
        {"market_trade_aggregates_1s", "core", "trade_1s_core_hours",
         "trade_1s_core_min_hours", 720, 168, "hours"},
        // Long-lived 1m/aggregate series are configured in DAYS (monthly
        // partitions).
        {"market_derivatives_context_1m", "all", "derivatives_context_days",
         "derivatives_context_min_days", 180, 90, "days"},
        {"market_bbo_1m", "all", "bbo_1m_days", "bbo_1m_min_days", 180, 90, "days"},
        {"market_trade_aggregates_1m", "all", "trade_1m_days", "trade_1m_min_days", 180, 90,
         "days"},
        {"market_book_aggregates_1m", "all", "book_1m_days", "book_1m_min_days", 180, 90, "days"},
        {"market_candles_1m", "all", "candles_1m_days", "candles_1m_min_days", 180, 90, "days"},
        {"market_candles_agg", "all", "candles_agg_days", "candles_agg_min_days", 365, 180,
         "days"},
    };

    persistence::with_txn_logged(pool, "reconcile_retention_policies", [&](pqxx::work& txn) {
        for (const auto& k : kKeys) {
            const int64_t target = config::opt_int(r, k.target_key, k.target_default);
            const int64_t minimum = config::opt_int(r, k.minimum_key, k.minimum_default);
            const std::string unit_suffix = std::string(" ") + k.unit;
            const std::string target_iv = std::to_string(target) + unit_suffix;
            const std::string minimum_iv = std::to_string(minimum) + unit_suffix;
            txn.exec_params(
                "UPDATE retention_policies SET target_keep = $3::interval, "
                "minimum_keep = LEAST($4::interval, $3::interval), updated_at = now() "
                "WHERE table_name = $1 AND retention_class = $2",
                k.table, k.cls, target_iv, minimum_iv);
        }
    });
}

// Reconcile the core-asset registry from config (retention.core_assets), so the
// core group can be tuned without a migration.
void reconcile_core_assets(persistence::PgPool& pool, const config::ServiceContext& ctx) {
    const auto r = ctx.root["retention"];
    if (!r || !r["core_assets"] || !r["core_assets"].IsSequence()) return;
    std::vector<std::string> bases;
    for (const auto& a : r["core_assets"]) {
        const std::string b = a.as<std::string>("");
        if (!b.empty()) bases.push_back(b);
    }
    if (bases.empty()) return;
    persistence::with_txn_logged(pool, "reconcile_core_assets", [&](pqxx::work& txn) {
        std::string in;
        for (const auto& b : bases) {
            if (!in.empty()) in += ",";
            in += txn.quote(b);
        }
        txn.exec("DELETE FROM retention_core_assets WHERE base_asset NOT IN (" + in + ")");
        for (const auto& b : bases) {
            txn.exec_params("INSERT INTO retention_core_assets (base_asset) VALUES ($1) "
                            "ON CONFLICT DO NOTHING",
                            b);
        }
    });
}

void publish_storage_event(messaging::NatsClient& nats, const char* kind,
                           const persistence::BudgetStatus& b, int dropped) {
    messaging::EnvelopeMeta meta;
    meta.event_type = std::string("storage.") + kind;
    meta.source_service = "retention_manager";
    meta.message_id = std::string("storage:") + kind + ":" + std::to_string(timeutil::now_ms());
    auto env = messaging::make_envelope(meta);
    auto* ev = env.mutable_integrity_event();
    ev->set_state(b.state == "NORMAL" || b.state == "WARNING" ? altspotter::v1::HEALTH_HEALTHY
                                                              : altspotter::v1::HEALTH_DEGRADED);
    ev->set_detail("db " + std::to_string(b.db_bytes / (1024 * 1024)) + "MiB / limit " +
                   std::to_string(b.hard_limit_bytes / (1024 * 1024)) + "MiB state=" + b.state +
                   " mode=" + std::string(persistence_mode_label(b.mode)) +
                   " dropped=" + std::to_string(dropped));
    (void)nats.js_publish(subjects::storage_event(kind), messaging::serialize(env),
                          env.message_id());
}

} // namespace

namespace altspotter::components {

int retention_manager(config::ServiceContext ctx) {
    const int64_t monitor_sec = config::opt_int(ctx.root["storage"], "monitor_interval_sec", 300);
    const int64_t routine_sec = config::opt_int(ctx.service, "routine_interval_sec", 3600);
    const auto rcfg = parse_retention_cfg(ctx);
    const auto ops_cfg = parse_ops_cfg(ctx);

    persistence::PgPool pool(
        ctx.common.postgres_dsn,
        static_cast<size_t>(config::opt_int(ctx.service, "postgres_pool_size", 2)));
    if (auto probe = pool.with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); }); !probe) {
        log::critical("postgres_unavailable", {{"message", probe.error().message}});
        return 1;
    }

    std::unique_ptr<messaging::NatsClient> nats;
    if (auto n = messaging::NatsClient::connect(ctx.common.nats_url, ctx.service_name); n) {
        nats = std::move(*n);
    } else {
        log::warn("retention_nats_unavailable", {{"message", n.error().message}});
    }

    reconcile_core_assets(pool, ctx);
    reconcile_policies(pool, ctx);
    if (auto c = persistence::ensure_partitions(pool); c) {
        log::info("retention_partitions_ensured", {{"created", *c}});
    }

    auto& shutdown = ShutdownToken::instance();
    std::string last_state = "NORMAL";
    int64_t last_routine = 0;
    int64_t last_partition_ensure = timeutil::now_ms();

    while (!shutdown.stop_requested()) {
        // Carry the previous state so the ladder applies its release margin
        // instead of re-classifying from scratch on every pass.
        auto budget = persistence::measure_budget(pool, rcfg, last_state);
        if (!budget) {
            log::error("retention_budget_failed", {{"message", budget.error().message}});
            if (shutdown.wait_for(std::chrono::seconds(monitor_sec))) break;
            continue;
        }
        PersistenceGate::instance().set(budget->mode);

        const int64_t now = timeutil::now_ms();
        // Hourly: raw partitions are hourly and pre-created 12 h ahead.
        if (now - last_partition_ensure >= 3600'000) {
            (void)persistence::ensure_partitions(pool);
            last_partition_ensure = now;
        }

        int dropped = 0;
        const bool pressure = budget->state == "CLEANUP" || budget->state == "CRITICAL" ||
                              budget->state == "HARD_LIMIT";
        const bool routine_due = now - last_routine >= routine_sec * 1000;
        if (pressure || routine_due) {
            auto run = pool.with_txn([&](pqxx::work& txn) {
                txn.exec_params(
                    "INSERT INTO retention_runs (mode, budget_state, db_bytes_before) "
                    "VALUES ($1, $2, $3)",
                    "scheduled", budget->state, budget->db_bytes);
            });
            (void)run;
            auto outcome = persistence::apply_retention(pool, rcfg, pressure, /*dry_run=*/false);
            if (outcome) {
                dropped = outcome->partitions_dropped;
                last_routine = now;
                if (dropped > 0) {
                    log::info("retention_dropped", {{"partitions", dropped},
                                                    {"state", budget->state},
                                                    {"aggressive", pressure}});
                }
                persistence::with_txn_logged(pool, "retention_run_finish",
                                                  [&](pqxx::work& txn) {
                    txn.exec_params(
                        "UPDATE retention_runs SET finished_at = now(), db_bytes_after = $1, "
                        "partitions_dropped = $2 WHERE id = (SELECT max(id) FROM retention_runs)",
                        outcome->bytes_after, dropped);
                });
            } else {
                log::error("retention_apply_failed", {{"message", outcome.error().message}});
            }
            // Unpartitioned audit/telemetry tables grow forever otherwise.
            (void)persistence::prune_operational_tables(pool, ops_cfg);
        }

        (void)persistence::record_statistics(pool, *budget);

        if (nats && (budget->state != last_state || dropped > 0)) {
            const char* kind = budget->state == "HARD_LIMIT" || budget->state == "CRITICAL"
                                   ? "critical"
                                   : dropped > 0 ? "cleanup" : "state";
            publish_storage_event(*nats, kind, *budget, dropped);
        }
        if (budget->state != last_state) {
            log::warn("storage_budget_state", {{"from", last_state},
                                               {"to", budget->state},
                                               {"db_mib", budget->db_bytes / (1024 * 1024)},
                                               {"mode", std::string(persistence_mode_label(
                                                            budget->mode))}});
            last_state = budget->state;
        }

        if (shutdown.wait_for(std::chrono::seconds(monitor_sec))) break;
    }

    if (nats) nats->drain(2000);
    log::info("service_stopped", {});
    return 0;
}

}  // namespace altspotter::components
