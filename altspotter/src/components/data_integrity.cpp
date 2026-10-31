// data-integrity-service: continuous quality control of the minute grid.
//
//   * refreshes history_coverage (earliest/latest/rows/gap counts);
//   * detects missing minutes via a generate_series anti-join, groups them
//     into ranges and records history_gaps (no silent synthetic candles);
//   * dispatches cmd.history.bitget.<sym>.repair with bounded attempts and
//     exponential backoff (no infinite repair loops);
//   * consumes evt.history.> to close or retry gaps;
//   * sanity checks: duplicate natural keys, OHLC violations, stale/unclosed
//     candles, revision storms;
//   * per-instrument health state machine (healthy/degraded/failed/recovered)
//     published to evt.integrity.bitget.<sym>.health;
//   * daily partition pre-creation via altspotter_ensure_candle_partitions().

#include "components/gap_repair_logic.hpp"
#include "components/components.hpp"
#include "core/readiness.hpp"
#include "core/shutdown.hpp"
#include "config/config.hpp"
#include "logging/logger.hpp"
#include "messaging/codec.hpp"
#include "messaging/nats_client.hpp"
#include "messaging/subjects.hpp"
#include "persistence/pg.hpp"
#include "persistence/txn_log.hpp"
#include "core/time.hpp"

#include <map>
#include <random>
#include <thread>

using namespace altspotter;
using namespace altspotter::integrity;

namespace {

struct InstrumentRow {
    int64_t instrument_id = 0;
    std::string symbol;
    int16_t exchange_id = 1;
    std::string code = "bitget";
};

struct GapRange {
    int64_t start_ms = 0;
    int64_t end_ms = 0; // inclusive
};

class IntegrityService {
public:
    IntegrityService(config::ServiceContext& ctx, persistence::PgPool& pool,
                     messaging::NatsClient& nats)
        : pool_(pool), nats_(nats), rng_(std::random_device{}()) {
        max_attempts_ = config::opt_int(ctx.service, "max_gap_repair_attempts", 5);
        backoff_base_sec_ = config::opt_int(ctx.service, "repair_backoff_base_sec", 120);
        backoff_max_sec_ = config::opt_int(ctx.service, "repair_backoff_max_sec", 7200);
        degraded_sec_ = config::opt_int(ctx.service, "latest_candle_degraded_sec", 300);
        failed_sec_ = config::opt_int(ctx.service, "latest_candle_failed_sec", 1800);
        max_gap_minutes_per_cmd_ = config::opt_int(ctx.service, "max_gap_minutes_per_command", 1000);
    }

    Result<std::vector<InstrumentRow>> load_active() {
        std::vector<InstrumentRow> out;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec(
                "SELECT ei.instrument_id, ei.exchange_symbol, ei.exchange_id, e.code "
                "FROM exchange_instruments ei JOIN exchanges e ON e.id = ei.exchange_id "
                "WHERE ei.is_whitelisted AND ei.status = 'active' "
                "ORDER BY ei.exchange_id, ei.exchange_symbol");
            for (const auto& r : rows) {
                out.push_back({r[0].as<int64_t>(), r[1].as<std::string>(),
                               r[2].as<int16_t>(), r[3].as<std::string>()});
            }
        });
        if (!res) return std::unexpected(res.error());
        return out;
    }

    // ---- coverage + gap scan --------------------------------------------------
    void scan_instrument(const InstrumentRow& instr) {
        std::vector<GapRange> new_gaps;
        int64_t row_count = 0;
        int64_t earliest = 0;
        int64_t latest = 0;

        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec("SET LOCAL TIME ZONE 'UTC'");
            const auto bounds = txn.exec_params(
                "SELECT (extract(epoch FROM min(open_time)) * 1000)::bigint, "
                "       (extract(epoch FROM max(open_time)) * 1000)::bigint, count(*) "
                "FROM market_candles_1m WHERE exchange_id = $1 AND instrument_id = $2 "
                "AND is_closed",
                instr.exchange_id, instr.instrument_id);
            if (bounds.empty() || bounds[0][0].is_null()) return;
            earliest = bounds[0][0].as<int64_t>();
            latest = bounds[0][1].as<int64_t>();
            row_count = bounds[0][2].as<int64_t>();

            // Missing-minute islands between earliest and latest.
            const auto gaps = txn.exec_params(
                "WITH grid AS ("
                "  SELECT generate_series(to_timestamp($3::bigint / 1000.0), "
                "         to_timestamp($4::bigint / 1000.0), "
                "         interval '1 minute') AS m"
                "), missing AS ("
                "  SELECT g.m FROM grid g LEFT JOIN market_candles_1m c "
                "    ON c.exchange_id = $1 AND c.instrument_id = $2 AND c.open_time = g.m "
                "  WHERE c.open_time IS NULL"
                "), islands AS ("
                "  SELECT m, m - (row_number() OVER (ORDER BY m)) * interval '1 minute' AS grp "
                "  FROM missing"
                ") SELECT (extract(epoch FROM min(m)) * 1000)::bigint, "
                "         (extract(epoch FROM max(m)) * 1000)::bigint "
                "FROM islands GROUP BY grp ORDER BY 1",
                instr.exchange_id, instr.instrument_id, earliest, latest);
            for (const auto& g : gaps) {
                new_gaps.push_back({g[0].as<int64_t>(), g[1].as<int64_t>()});
            }

            // Record new gaps (dedup by unique key).
            for (const auto& g : new_gaps) {
                txn.exec_params(
                    "INSERT INTO history_gaps (exchange_id, instrument_id, timeframe, gap_start, "
                    "gap_end, expected_rows) VALUES ($1, $2, '1m', "
                    "to_timestamp($3::bigint / 1000.0), "
                    "to_timestamp($4::bigint / 1000.0), $5) "
                    "ON CONFLICT (exchange_id, instrument_id, timeframe, gap_start) DO UPDATE SET "
                    "gap_end = EXCLUDED.gap_end, expected_rows = EXCLUDED.expected_rows "
                    "WHERE history_gaps.status <> 'repaired'",
                    instr.exchange_id, instr.instrument_id, g.start_ms, g.end_ms,
                    (g.end_ms - g.start_ms) / timeutil::kMsPerMinute + 1);
            }

            // Close gaps whose ranges are now fully present.
            txn.exec_params(
                "UPDATE history_gaps hg SET status = 'repaired', resolved_at = now() "
                "WHERE hg.exchange_id = $1 AND hg.instrument_id = $2 AND hg.timeframe = '1m' "
                "AND hg.status IN ('open', 'repairing') AND NOT EXISTS ("
                "  SELECT 1 FROM generate_series(hg.gap_start, hg.gap_end, interval '1 minute') s "
                "  LEFT JOIN market_candles_1m c ON c.exchange_id = $1 AND c.instrument_id = $2 "
                "    AND c.open_time = s WHERE c.open_time IS NULL)",
                instr.exchange_id, instr.instrument_id);

            // Refresh coverage.
            txn.exec_params(
                "INSERT INTO history_coverage (exchange_id, instrument_id, timeframe, actual_from, "
                "covered_until, row_count, gap_count, open_gap_count, status) "
                "VALUES ($1, $2, '1m', to_timestamp($3::bigint / 1000.0), "
                "to_timestamp($4::bigint / 1000.0), $5, "
                " (SELECT count(*) FROM history_gaps WHERE exchange_id = $1 AND instrument_id = $2 "
                "   AND timeframe = '1m'), "
                " (SELECT count(*) FROM history_gaps WHERE exchange_id = $1 AND instrument_id = $2 "
                "   AND timeframe = '1m' AND status IN ('open', 'repairing')), 'pending') "
                "ON CONFLICT (exchange_id, instrument_id, timeframe) DO UPDATE SET "
                "actual_from = EXCLUDED.actual_from, covered_until = EXCLUDED.covered_until, "
                "row_count = EXCLUDED.row_count, gap_count = EXCLUDED.gap_count, "
                "open_gap_count = EXCLUDED.open_gap_count, updated_at = now()",
                instr.exchange_id, instr.instrument_id, earliest, latest, row_count);
        });
        if (!res) {
            log::error("gap_scan_failed", {{"symbol", instr.symbol},
                                           {"message", res.error().message}});
            return;
        }
        if (!new_gaps.empty()) {
            log::warn("gaps_detected", {{"symbol", instr.symbol},
                                        {"gap_ranges", new_gaps.size()},
                                        {"rows", row_count}});
        }
    }

    // ---- repair dispatch --------------------------------------------------------
    void dispatch_repairs() {
        struct PendingGap {
            int64_t id;
            int64_t instrument_id;
            std::string symbol;
            int64_t start_ms;
            int64_t end_ms;
            int attempts;
            int16_t exchange_id;
            std::string code;
            int64_t cursor_ms;   // oldest missing minute at the last dispatch, 0 if never
        };
        std::vector<PendingGap> pending;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec_params(
                "SELECT hg.id, hg.instrument_id, ei.exchange_symbol, "
                "       (extract(epoch FROM hg.gap_start) * 1000)::bigint, "
                "       (extract(epoch FROM hg.gap_end) * 1000)::bigint, hg.attempts, "
                "       hg.exchange_id, e.code, "
                "       coalesce((extract(epoch FROM hg.repair_cursor) * 1000)::bigint, 0) "
                "FROM history_gaps hg "
                "JOIN exchange_instruments ei ON ei.exchange_id = hg.exchange_id "
                "  AND ei.instrument_id = hg.instrument_id "
                "JOIN exchanges e ON e.id = hg.exchange_id "
                "WHERE hg.status IN ('open', 'repairing') AND hg.next_attempt_at <= now() "
                "  AND hg.timeframe = '1m' AND hg.attempts < $1 "
                // Oldest DUE first, not oldest gap first. Ordering by gap_start
                // let one multi-day outage hold all 20 slots indefinitely while
                // freshly detected gaps sat untouched for hours; backoff already
                // spaces out anything just dispatched, so next_attempt_at is a
                // fair queue.
                "ORDER BY hg.next_attempt_at, hg.gap_start LIMIT 20",
                max_attempts_);
            for (const auto& r : rows) {
                pending.push_back({r[0].as<int64_t>(), r[1].as<int64_t>(), r[2].as<std::string>(),
                                   r[3].as<int64_t>(), r[4].as<int64_t>(), r[5].as<int>(),
                                   r[6].as<int16_t>(), r[7].as<std::string>(),
                                   r[8].as<int64_t>()});
            }
            // Exhausted gaps become unrepairable (kept for audit, no infinite loop).
            txn.exec_params(
                "UPDATE history_gaps SET status = 'unrepairable', "
                "last_error = COALESCE(last_error, 'max repair attempts reached') "
                "WHERE status IN ('open', 'repairing') AND attempts >= $1",
                max_attempts_);
        });
        if (!res) {
            log::error("repair_dispatch_query_failed", {{"message", res.error().message}});
            return;
        }

        for (const auto& gap : pending) {
            // Split long gaps into bounded commands.  The chunk must begin at
            // the oldest minute STILL missing, not at gap_start: a gap longer
            // than one command is repaired in several passes, and re-requesting
            // the already-recovered head made the remainder unreachable.
            int64_t from_ms = 0;
            const bool located = pool_.with_txn([&](pqxx::work& txn) {
                const auto row = txn.exec_params1(
                    "SELECT coalesce((extract(epoch FROM min(s)) * 1000)::bigint, 0) "
                    "FROM generate_series(to_timestamp($3::bigint / 1000.0), "
                    "                     to_timestamp($4::bigint / 1000.0), "
                    "                     interval '1 minute') s "
                    "LEFT JOIN market_candles_1m c ON c.exchange_id = $1 "
                    "  AND c.instrument_id = $2 AND c.open_time = s "
                    "WHERE c.open_time IS NULL",
                    gap.exchange_id, gap.instrument_id, gap.start_ms, gap.end_ms);
                from_ms = row[0].as<int64_t>();
            }).has_value();
            if (!located) {
                log::error("repair_cursor_query_failed",
                           {{"symbol", gap.symbol}, {"gap_id", gap.id}});
                continue;
            }
            if (from_ms == 0) {
                // Nothing is missing any more; the detector's own close pass
                // will mark it repaired on its next sweep.
                continue;
            }
            // Progress since the last dispatch means the gap is not stuck, so
            // the attempt budget — which exists to retire genuinely
            // unrecoverable ranges — starts again from this window.
            const bool advanced = from_ms > gap.cursor_ms;
            const int64_t cmd_end =
                repair_window_end(from_ms, gap.end_ms, max_gap_minutes_per_cmd_);
            const int attempt = next_attempt_number(advanced, gap.attempts);

            // No repair command may trigger a REST fetch into a retention-
            // dropped month. Do not create a partition or burn attempts: the
            // owner must first approve any recovery beyond current capacity.
            bool partitions_present = false;
            const auto partition_probe = pool_.with_txn([&](pqxx::work& txn) {
                const auto row = txn.exec_params1(
                    std::string(kRepairPartitionsPresentSql), from_ms, cmd_end);
                partitions_present = row[0].as<bool>();
            });
            if (!partition_probe) {
                log::error("repair_partition_probe_failed",
                           {{"symbol", gap.symbol}, {"gap_id", gap.id},
                            {"message", partition_probe.error().message}});
                continue;
            }
            if (!partitions_present) {
                log::warn("repair_deferred_partition_missing",
                          {{"symbol", gap.symbol}, {"gap_id", gap.id},
                           {"venue", gap.code}});
                continue;
            }

            messaging::EnvelopeMeta meta;
            meta.event_type = "history.repair.command";
            meta.exchange_id = gap.exchange_id;
            meta.instrument_id = gap.instrument_id;
            meta.exchange_symbol = gap.symbol;
            meta.source_service = "data_integrity";
            meta.message_id = repair_message_id(gap.id, from_ms, attempt);
            auto env = messaging::make_envelope(meta);
            auto* cmd = env.mutable_repair_command();
            cmd->set_gap_id(gap.id);
            cmd->set_timeframe(altspotter::v1::TF_1M);
            cmd->set_from_ms(from_ms);
            cmd->set_to_ms(cmd_end);
            cmd->set_attempt(attempt);

            auto pub = nats_.js_publish(subjects::repair_command(gap.code, gap.symbol),
                                        messaging::serialize(env), env.message_id());
            if (!pub) {
                log::error("repair_command_publish_failed", {{"symbol", gap.symbol},
                                                             {"message", pub.error().message}});
                continue;
            }
            // Backoff with jitter for the next attempt if this one does not resolve.
            const int64_t ceiling = backoff_ceiling_sec(advanced, gap.attempts,
                                                        backoff_base_sec_, backoff_max_sec_);
            std::uniform_int_distribution<int64_t> dist(backoff_base_sec_,
                                                        std::max(backoff_base_sec_, ceiling));
            const int64_t next_sec = dist(rng_);
            const bool dispatch_recorded = persistence::with_txn_logged(
                pool_, "repair_dispatch_mark", [&](pqxx::work& txn) {
                    txn.exec_params(
                        "UPDATE history_gaps SET status = 'repairing', "
                        "attempts = $3, "
                        "repair_cursor = to_timestamp($4::bigint / 1000.0), "
                        "next_attempt_at = now() + make_interval(secs => $2) WHERE id = $1",
                        gap.id, static_cast<double>(next_sec), attempt, from_ms);
                });
            if (!dispatch_recorded) {
                // The command has a deterministic message id.  Leaving the DB
                // row unchanged causes a safe JetStream-deduplicated retry.
                log::error("repair_dispatch_not_recorded",
                           {{"symbol", gap.symbol}, {"gap_id", gap.id}});
                continue;
            }
            log::info("repair_dispatched", {{"symbol", gap.symbol},
                                            {"gap_id", gap.id},
                                            {"attempt", gap.attempts + 1},
                                            {"next_retry_sec", next_sec}});
        }
    }

    bool handle_repair_result(const altspotter::v1::Envelope& env) {
        const auto& res = env.repair_result();
        return persistence::with_txn_logged(pool_, "repair_result_apply", [&](pqxx::work& txn) {
            if (res.status() == altspotter::v1::REPAIR_STATUS_FAILED) {
                txn.exec_params(
                    "UPDATE history_gaps SET status = 'open', last_error = $2 WHERE id = $1",
                    res.gap_id(), res.error().substr(0, 500));
            } else {
                // completed/partial: the next scan verifies actual completeness;
                // a fully-repaired range is auto-closed there.
                txn.exec_params(
                    "UPDATE history_gaps SET last_error = NULL WHERE id = $1", res.gap_id());
            }
        });
    }

    // ---- health + sanity ---------------------------------------------------------
    bool publish_health_transition(const InstrumentRow& instr, const std::string& previous,
                                   const std::string& state, int64_t age_sec,
                                   int64_t open_gaps, const std::string& detail) {
        const std::string kind = (state == "healthy") ? "recovered" : state;
        messaging::EnvelopeMeta meta;
        meta.event_type = "integrity.health." + kind;
        meta.exchange_id = instr.exchange_id;
        meta.instrument_id = instr.instrument_id;
        meta.exchange_symbol = instr.symbol;
        meta.source_service = "data_integrity";
        meta.message_id = "health:" + instr.code + ":" + instr.symbol + ":" + kind + ":" +
                          std::to_string(timeutil::now_ms() / 60000);
        auto env = messaging::make_envelope(meta);
        auto* ev = env.mutable_integrity_event();
        ev->set_state(state == "healthy"    ? altspotter::v1::HEALTH_RECOVERED
                      : state == "degraded" ? altspotter::v1::HEALTH_DEGRADED
                                            : altspotter::v1::HEALTH_FAILED);
        ev->set_detail(detail);
        ev->set_open_gaps(open_gaps);
        ev->set_latest_candle_age_sec(age_sec);
        const auto published = nats_.js_publish(
            subjects::integrity_event(instr.code, instr.symbol, "health"),
            messaging::serialize(env), env.message_id());
        if (!published) {
            log::error("health_transition_publish_failed",
                       {{"symbol", instr.symbol},
                        {"to", state},
                        {"message", published.error().message}});
            return false;
        }
        log::warn("health_transition", {{"symbol", instr.symbol},
                                        {"from", previous},
                                        {"to", state},
                                        {"latest_age_sec", age_sec},
                                        {"open_gaps", open_gaps}});
        return true;
    }

    void health_cycle(const std::vector<InstrumentRow>& instruments) {
        for (const auto& instr : instruments) {
            int64_t latest_ms = 0;
            int64_t open_gaps = 0;
            auto res = pool_.with_txn([&](pqxx::work& txn) {
                const auto r = txn.exec_params(
                    "SELECT COALESCE((extract(epoch FROM max(open_time)) * 1000)::bigint, 0) "
                    "FROM market_candles_1m WHERE exchange_id = $1 AND instrument_id = $2",
                    instr.exchange_id, instr.instrument_id);
                latest_ms = r[0][0].as<int64_t>();
                const auto g = txn.exec_params(
                    "SELECT count(*) FROM history_gaps WHERE exchange_id = $1 "
                    "AND instrument_id = $2 AND status IN ('open', 'repairing')",
                    instr.exchange_id, instr.instrument_id);
                open_gaps = g[0][0].as<int64_t>();
            });
            if (!res) {
                log::error("integrity_health_query_failed",
                           {{"symbol", instr.symbol}, {"message", res.error().message}});
                auto& prev = health_state_[instr.code + ":" + instr.symbol];
                if (prev.empty()) prev = "healthy";
                if (prev != "degraded" &&
                    publish_health_transition(instr, prev, "degraded", -1, 0,
                                              "database health read failed")) {
                    prev = "degraded";
                }
                continue;
            }

            const int64_t age_sec =
                latest_ms == 0 ? -1 : (timeutil::now_ms() - latest_ms) / 1000;
            std::string state = "healthy";
            if (latest_ms == 0) {
                state = "degraded"; // no data yet (bootstrap pending)
            } else if (age_sec > failed_sec_) {
                state = "failed";
            } else if (age_sec > degraded_sec_ || open_gaps > 0) {
                state = "degraded";
            }

            auto& prev = health_state_[instr.symbol];
            if (prev.empty()) prev = "healthy";
            if (state != prev) {
                const std::string detail =
                    "latest 1m age " + std::to_string(age_sec) + "s, open gaps " +
                    std::to_string(open_gaps);
                if (publish_health_transition(instr, prev, state, age_sec, open_gaps, detail)) {
                    prev = state;
                }
            }
        }
    }

    void sanity_checks() {
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto dup = txn.exec(
                "SELECT count(*) FROM (SELECT exchange_id, instrument_id, open_time "
                "FROM market_candles_1m GROUP BY 1, 2, 3 HAVING count(*) > 1) d");
            const int64_t dups = dup[0][0].as<int64_t>();
            if (dups > 0) {
                log::error("duplicate_natural_keys", {{"count", dups}});
            }
            const auto unclosed = txn.exec(
                "SELECT count(*) FROM market_candles_1m "
                "WHERE NOT is_closed AND open_time < now() - interval '10 minutes'");
            const int64_t stale_open = unclosed[0][0].as<int64_t>();
            if (stale_open > 0) {
                log::warn("stale_unclosed_candles", {{"count", stale_open}});
            }
            const auto storm = txn.exec(
                "SELECT count(*) FROM market_candles_1m WHERE revision > 20");
            const int64_t storms = storm[0][0].as<int64_t>();
            if (storms > 0) {
                log::warn("revision_storm_suspected", {{"count", storms}});
            }
        });
        if (!res) {
            log::warn("sanity_check_failed", {{"message", res.error().message}});
        }
    }

    void ensure_partitions() {
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const int64_t now = timeutil::now_ms();
            txn.exec_params("SELECT altspotter_ensure_candle_partitions("
                            "to_timestamp($1::bigint / 1000.0), "
                            "to_timestamp($2::bigint / 1000.0))",
                            now - 120LL * 86'400'000, now + 92LL * 86'400'000);
        });
        if (!res) {
            log::error("partition_ensure_failed", {{"message", res.error().message}});
        } else {
            log::info("partitions_ensured", {});
        }
    }

private:
    persistence::PgPool& pool_;
    messaging::NatsClient& nats_;
    std::mt19937_64 rng_;
    std::map<std::string, std::string> health_state_;
    int64_t max_attempts_ = 5;
    int64_t backoff_base_sec_ = 120;
    int64_t backoff_max_sec_ = 7200;
    int64_t degraded_sec_ = 300;
    int64_t failed_sec_ = 1800;
    int64_t max_gap_minutes_per_cmd_ = 1000;
};

} // namespace

namespace altspotter::components {

int data_integrity(config::ServiceContext ctx) {
    const int64_t gap_scan_interval_sec =
        config::opt_int(ctx.service, "gap_scan_interval_sec", 600);
    const int64_t coverage_interval_sec =
        config::opt_int(ctx.service, "coverage_refresh_interval_sec", 300);
    const int64_t partition_interval_sec =
        config::opt_int(ctx.service, "partition_ensure_interval_sec", 86400);

    auto nats = messaging::NatsClient::connect(ctx.common.nats_url, ctx.service_name);
    if (!nats) {
        log::critical("nats_unavailable", {{"message", nats.error().message}});
        return 1;
    }
    persistence::PgPool pool(
        ctx.common.postgres_dsn,
        static_cast<size_t>(config::opt_int(ctx.service, "postgres_pool_size", 2)));
    if (auto probe = pool.with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); }); !probe) {
        log::critical("postgres_unavailable", {{"message", probe.error().message}});
        return 1;
    }

    IntegrityService service(ctx, pool, **nats);
    auto& shutdown = ShutdownToken::instance();

    // Repair-result consumer.
    std::thread events_thread([&] {
        auto consumer = (*nats)->pull_consumer("ALTSPOTTER_CONTROL", "integrity-repair-events");
        if (!consumer) {
            log::error("integrity_consumer_failed", {{"message", consumer.error().message}});
            return;
        }
        while (!shutdown.stop_requested()) {
            auto msgs = consumer->fetch(16, 2000);
            if (!msgs) {
                if (shutdown.wait_for(std::chrono::seconds(2))) break;
                continue;
            }
            for (auto& msg : *msgs) {
                auto env = messaging::parse_envelope(msg.data());
                if (!env) {
                    log::warn("integrity_repair_result_parse_failed",
                              {{"message", env.error().message}});
                    (void)msg.term();
                } else if (!env->has_repair_result()) {
                    log::warn("integrity_repair_result_unsupported", {});
                    (void)msg.term();
                } else if (service.handle_repair_result(*env)) {
                    (void)msg.ack();
                } else {
                    (void)msg.nak(1000);
                }
            }
        }
    });

    service.ensure_partitions();
    int64_t last_scan = 0;
    int64_t last_health = 0;
    int64_t last_partitions = timeutil::now_ms();

    while (!shutdown.wait_for(std::chrono::seconds(5))) {
        const int64_t now = timeutil::now_ms();
        if (now - last_health >= coverage_interval_sec * 1000) {
            auto instruments = service.load_active();
            if (instruments) {
                service.health_cycle(*instruments);
            }
            last_health = now;
        }
        if (now - last_scan >= gap_scan_interval_sec * 1000) {
            auto instruments = service.load_active();
            if (instruments) {
                const int64_t started = timeutil::now_ms();
                for (const auto& instr : *instruments) {
                    if (shutdown.stop_requested()) break;
                    service.scan_instrument(instr);
                }
                // A stop ends the pass here. What follows is whole-table work --
                // on VPS1 the duplicate check alone is an 11 s aggregate over
                // every candle partition, the revision count another 9 s, plus
                // one query per open gap -- and running it after SIGTERM took
                // the process past TimeoutStopSec=60: systemd SIGKILLed it with
                // every other component already stopped. A pass takes ~500 s of
                // its 600 s interval, so a stop almost always lands inside one.
                if (shutdown.stop_requested()) break;
                service.dispatch_repairs();
                service.sanity_checks();
                log::info("gap_scan_done", {{"instruments", instruments->size()},
                                            {"duration_ms", timeutil::now_ms() - started}});
            } else {
                log::error("integrity_active_load_failed",
                           {{"message", instruments.error().message}});
            }
            last_scan = now;
        }
        if (now - last_partitions >= partition_interval_sec * 1000) {
            service.ensure_partitions();
            last_partitions = now;
        }
    }

    events_thread.join();
    (*nats)->drain(3000);
    log::info("service_stopped", {});
    return 0;
}

}  // namespace altspotter::components
