// market_history: REST bootstrap, incremental reconciliation and gap repair
// for 1m candle history, all venues.
//
//   bootstrap : walk the venue's history endpoint backwards from now to
//               now - bootstrap_days (or the listing date / actual exchange
//               boundary), publishing
//               CandleBatch messages to JetStream bar.bitget.<sym>.1m.backfill;
//               progress recorded in ingestion_runs + history_coverage.
//   reconcile : every reconcile_interval_sec re-fetch the last
//               reconcile_overlap_minutes closed minutes via /candles and
//               publish to bar.bitget.<sym>.1m.recon (REST confirms/corrects
//               WebSocket candles; the writer bumps `revision` on changes).
//   repair    : durable JetStream consumer on cmd.history.> executes
//               RepairCommand ranges and reports
//               evt.history.bitget.<sym>.repair.{completed,failed}.
//
// The live gateways are separate components and keep streaming during backfill.

#include "components/components.hpp"
#include "components/history_owner.hpp"
#include "core/readiness.hpp"
#include "core/shutdown.hpp"
#include "config/config.hpp"
#include "exchanges/adapter_factory.hpp"
#include "exchanges/bitget/bitget_adapter.hpp"
#include "exchanges/pagination.hpp"
#include "exchanges/venues.hpp"
#include "logging/logger.hpp"
#include "messaging/codec.hpp"
#include "messaging/nats_client.hpp"
#include "messaging/subjects.hpp"
#include "persistence/pg.hpp"
#include "core/time.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <thread>

using namespace altspotter;

namespace {

struct InstrumentRow {
    int64_t instrument_id = 0;
    std::string symbol;
    int64_t list_time_ms = 0;
};

Result<std::vector<InstrumentRow>> load_active(persistence::PgPool& pool, int16_t exchange_id) {
    std::vector<InstrumentRow> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT ei.instrument_id, ei.exchange_symbol, "
            "       COALESCE((extract(epoch FROM ei.list_time) * 1000)::bigint, 0) "
            "FROM exchange_instruments ei "
            "WHERE ei.exchange_id = $1 AND ei.is_whitelisted AND ei.status = 'active' "
            "ORDER BY ei.exchange_symbol",
            exchange_id);
        for (const auto& r : rows) {
            out.push_back({r[0].as<int64_t>(), r[1].as<std::string>(), r[2].as<int64_t>()});
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

class HistoryService {
public:
    // `owner` is the only session run state is written on (history_owner.hpp);
    // `stop` is true on shutdown AND once that ownership is lost.
    HistoryService(config::ServiceContext& ctx, persistence::PgPool& pool,
                   messaging::NatsClient& nats, exchanges::ExchangeMarketDataAdapter& adapter,
                   int16_t exchange_id, std::string code, history::RunOwner& owner,
                   std::function<bool()> stop)
        : ctx_(ctx), pool_(pool), nats_(nats), adapter_(adapter), owner_(owner),
          stop_(std::move(stop)), exchange_id_(exchange_id), code_(std::move(code)) {
        bootstrap_days_ = config::opt_int(ctx.service, "bootstrap_days", 180);
        publish_batch_size_ =
            static_cast<size_t>(config::opt_int(ctx.service, "publish_batch_size", 1000));
        reconcile_overlap_min_ = config::opt_int(ctx.service, "reconcile_overlap_minutes", 30);
    }

    // ---- publishing ---------------------------------------------------------
    Result<int64_t> publish_candles(const InstrumentRow& instr, std::vector<Candle> candles,
                                    CandleSource source, const char* kind,
                                    const std::string& run_tag) {
        int64_t published = 0;
        for (size_t off = 0; off < candles.size(); off += publish_batch_size_) {
            const size_t end = std::min(candles.size(), off + publish_batch_size_);
            messaging::EnvelopeMeta meta;
            meta.event_type = std::string("bar.batch.") + kind;
            meta.instrument_id = instr.instrument_id;
            meta.exchange_symbol = instr.symbol;
            meta.exchange_id = exchange_id_;
            meta.source_service = "market_history";
            // Venue code in the dedup id: Binance and Bybit share symbol strings
            // (BTCUSDT) but publish to the same ALTSPOTTER_BARS stream.
            meta.message_id = "hb:" + code_ + ":" + std::string(kind) + ":" + instr.symbol + ":" +
                              std::to_string(candles[off].open_time_ms) + ":" +
                              std::to_string(candles[end - 1].open_time_ms) + ":" + run_tag;
            auto env = messaging::make_envelope(meta);
            auto* batch = env.mutable_candle_batch();
            for (size_t i = off; i < end; ++i) {
                Candle c = candles[i];
                c.source = source;
                messaging::to_proto(c, batch->add_candles());
            }
            auto r = nats_.js_publish(subjects::bar_batch(code_, instr.symbol, Timeframe::m1,
                                                          kind),
                                      messaging::serialize(env), env.message_id(), 15000);
            if (!r) return std::unexpected(r.error());
            published += static_cast<int64_t>(end - off);
        }
        return published;
    }

    // ---- bootstrap ----------------------------------------------------------
    bool needs_bootstrap(const InstrumentRow& instr) {
        bool needed = true;
        const int64_t target_from =
            timeutil::floor_minute(timeutil::now_ms() - bootstrap_days_ * 86'400'000LL);
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec_params(
                "SELECT status, updated_at < now() - interval '30 minutes', "
                "       boundary_reached, "
                "       (extract(epoch FROM requested_from) * 1000)::bigint "
                "FROM history_coverage "
                "WHERE exchange_id = $1 AND instrument_id = $2 AND timeframe = '1m'",
                exchange_id_, instr.instrument_id);
            if (!rows.empty()) {
                const auto status = rows[0][0].as<std::string>();
                const bool stale = rows[0][1].as<bool>();
                // 'bootstrapping' older than 30 min = crashed run; resume it.
                needed = (status == "pending" || status == "failed" ||
                          (status == "bootstrapping" && stale));

                // Raising bootstrap_days must actually deepen the history. A
                // completed run leaves status='ready', so without this a
                // configured increase is silently ignored and every instrument
                // keeps whatever depth it happened to get. Re-run only when the
                // venue has more to give: boundary_reached means the walk hit
                // the exchange's own limit, and going again would just re-read
                // the same first page forever.
                if (!needed && status == "ready" && !rows[0][2].as<bool>(false) &&
                    !rows[0][3].is_null()) {
                    const int64_t requested = rows[0][3].as<int64_t>();
                    // One day of slack: the target moves with wall-clock time.
                    if (requested > target_from + 86'400'000LL) {
                        log::info("bootstrap_deepening",
                                  {{"symbol", instr.symbol},
                                   {"had_from", timeutil::to_iso8601(requested)},
                                   {"want_from", timeutil::to_iso8601(target_from)}});
                        needed = true;
                    }
                }
            }
        });
        if (!res) {
            log::warn("coverage_check_failed", {{"symbol", instr.symbol},
                                                {"message", res.error().message}});
            return false;
        }
        return needed;
    }

    // A venue rate limit during a long backfill is throttling, not failure.
    static constexpr int kMaxRateLimitPauses = 10;
    static constexpr int kRateLimitPauseSec = 30;

    // Create every candle partition covering [from_ms, to_ms] before writing
    // into it. Idempotent: the SQL function creates only what is missing.
    bool ensure_partitions_for(int64_t from_ms, int64_t to_ms) {
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec_params("SELECT altspotter_ensure_candle_partitions("
                            "to_timestamp($1::bigint / 1000.0), "
                            "to_timestamp($2::bigint / 1000.0))",
                            from_ms, to_ms);
        });
        if (!res) {
            log::error("partition_ensure_failed", {{"message", res.error().message}});
            return false;
        }
        return true;
    }

    void bootstrap_instrument(const InstrumentRow& instr) {
        const int64_t now = timeutil::now_ms();
        const int64_t last_closed = timeutil::floor_minute(now) - timeutil::kMsPerMinute;
        int64_t target_from = timeutil::floor_minute(now - bootstrap_days_ * 86'400'000LL);
        if (instr.list_time_ms > 0 && instr.list_time_ms > target_from) {
            target_from = timeutil::floor_minute(instr.list_time_ms);
        }

        // Partitions must exist before the first page lands. data_integrity
        // pre-creates a fixed [now-120d, now+92d] window, which silently fails
        // to cover a deeper bootstrap: measured on VPS1, a 90-day backfill into
        // a database whose June partition had been dropped produced 6540
        // `no partition of relation "market_candles_1m" found for row`
        // rejections -- the rows were journalled, not stored, and were lost.
        // The backfill knows its own reach, so it guarantees its own coverage.
        if (!ensure_partitions_for(target_from, now)) {
            log::error("bootstrap_partitions_unavailable",
                       {{"symbol", instr.symbol},
                        {"from", timeutil::to_iso8601(target_from)}});
            return;
        }

        int64_t run_id = 0;
        const bool start_recorded = owner_.with_txn("bootstrap_start", [&](pqxx::work& txn) {
            const auto r = txn.exec_params(
                "INSERT INTO ingestion_runs (service, kind, exchange_id, instrument_id, "
                "timeframe, requested_from, requested_to) "
                "VALUES ('market-history-service', 'bootstrap', $1, $2, '1m', "
                "to_timestamp($3::bigint / 1000.0), to_timestamp($4::bigint / 1000.0)) RETURNING id",
                exchange_id_, instr.instrument_id, target_from, now);
            run_id = r[0][0].as<int64_t>();
            txn.exec_params(
                "INSERT INTO history_coverage (exchange_id, instrument_id, timeframe, "
                "requested_from, status) VALUES ($1, $2, '1m', to_timestamp($3::bigint / 1000.0), "
                "'bootstrapping') "
                "ON CONFLICT (exchange_id, instrument_id, timeframe) DO UPDATE SET "
                "requested_from = EXCLUDED.requested_from, status = 'bootstrapping', "
                "updated_at = now()",
                exchange_id_, instr.instrument_id, target_from);
        });
        if (!start_recorded) {
            // Without a durable run id/progress row we cannot publish an
            // auditable, idempotently tagged bootstrap stream.
            log::error("bootstrap_start_not_recorded", {{"symbol", instr.symbol}});
            return;
        }

        log::info("bootstrap_started", {{"symbol", instr.symbol},
                                        {"run_id", run_id},
                                        {"from", timeutil::to_iso8601(target_from)},
                                        {"to", timeutil::to_iso8601(last_closed)}});

        int64_t cursor_end = last_closed + timeutil::kMsPerMinute; // exclusive
        int64_t earliest_seen = 0;
        int64_t latest_seen = 0;
        int64_t rows_received = 0;
        int64_t rows_published = 0;
        int request_count = 0;
        int error_count = 0;
        int empty_streak = 0;
        int rate_limit_pauses = 0;
        bool boundary_reached = false;
        std::string failure;

        while (!stop_()) {
            if (cursor_end <= target_from) break;
            auto page = adapter_.fetch_candles(instr.symbol, Timeframe::m1, target_from,
                                               cursor_end, exchanges::CandleQueryKind::historical);
            ++request_count;
            if (!page) {
                if (page.error().code == ErrorCode::shutdown_requested) {
                    failure = "shutdown";
                    break;
                }
                ++error_count;
                // A rate limit is the venue asking us to slow down, not a
                // reason to abandon 200 days of history. The adapter's own
                // retries are sized for a transient blip; a sustained backfill
                // can exhaust them and lose the whole instrument -- measured
                // here, Bybit answered `http=200 retCode=10006 Too many visits`
                // after 34 pages and NEARUSDT alone was left `failed` while its
                // 26 siblings completed. Back off well past the adapter's own
                // window and carry on from the same cursor.
                if (page.error().code == ErrorCode::rate_limited &&
                    rate_limit_pauses < kMaxRateLimitPauses) {
                    ++rate_limit_pauses;
                    log::warn("bootstrap_rate_limited",
                              {{"symbol", instr.symbol},
                               {"pause", std::to_string(rate_limit_pauses)},
                               {"of", std::to_string(kMaxRateLimitPauses)}});
                    if (pause(std::chrono::milliseconds(kRateLimitPauseSec * 1000))) {
                        failure = "shutdown";
                        break;
                    }
                    continue;
                }
                failure = page.error().message;
                log::error("bootstrap_page_failed", {{"symbol", instr.symbol},
                                                     {"message", failure},
                                                     {"error_code", page.error().code_name()}});
                break;
            }
            if (page->candles.empty()) {
                // A single empty response is not proof of the exchange boundary:
                // require three consecutive empties before accepting it.
                ++empty_streak;
                if (empty_streak >= 3) {
                    boundary_reached = true;
                    break;
                }
                continue;
            }
            empty_streak = 0;
            const int64_t page_earliest = page->candles.front().open_time_ms;
            const int64_t page_latest = page->candles.back().open_time_ms;
            rows_received += static_cast<int64_t>(page->candles.size());
            if (earliest_seen == 0 || page_earliest < earliest_seen) earliest_seen = page_earliest;
            if (page_latest > latest_seen) latest_seen = page_latest;

            auto pub = publish_candles(instr, std::move(page->candles),
                                       CandleSource::rest_bootstrap, "backfill",
                                       std::to_string(run_id));
            if (!pub) {
                failure = pub.error().message;
                log::error("bootstrap_publish_failed",
                           {{"symbol", instr.symbol}, {"message", failure}});
                break;
            }
            rows_published += *pub;

            auto next = exchanges::advance_history_cursor(page_earliest, cursor_end);
            if (!next) {
                failure = next.error().message;
                log::error("bootstrap_cursor_stuck", {{"symbol", instr.symbol},
                                                      {"cursor", cursor_end}});
                break;
            }
            cursor_end = *next; // endTime is exclusive on the next page
            if (exchanges::history_walk_done(page_earliest, target_from)) break;
        }

        const bool aborted = stop_() || failure == "shutdown";
        const bool ok = failure.empty() && !aborted;
        // Only a run still 'running' is finished: one closed by a newer owner
        // (history_runs.hpp) keeps that owner's verdict.
        bool fenced = false;
        const bool finish_recorded = owner_.with_txn("bootstrap_finish", [&](pqxx::work& txn) {
            fenced = txn.exec_params(history::kFinishBootstrapRunSql, run_id,
                                     ok ? "completed" : (aborted ? "aborted" : "failed"), failure,
                                     earliest_seen, latest_seen, rows_received, rows_published,
                                     request_count, error_count)
                         .affected_rows() == 0;
            if (fenced) return;
            txn.exec_params(
                "UPDATE history_coverage SET "
                "actual_from = CASE WHEN $3::bigint > 0 THEN to_timestamp($3::bigint / 1000.0) ELSE actual_from END, "
                "covered_until = CASE WHEN $4::bigint > 0 THEN to_timestamp($4::bigint / 1000.0) ELSE covered_until END, "
                "boundary_reached = boundary_reached OR $5, "
                "status = $6, "
                "bootstrap_completed_at = CASE WHEN $6 = 'ready' THEN now() END, "
                "updated_at = now() "
                "WHERE exchange_id = $1 AND instrument_id = $2 AND timeframe = '1m'",
                exchange_id_, instr.instrument_id, earliest_seen, latest_seen,
                boundary_reached, ok ? "ready" : (aborted ? "pending" : "failed"));
        });
        if (!finish_recorded || fenced) {
            // The run stays as the database has it: 'running' until the next
            // owner closes it, or already closed by a newer owner.
            log::error("bootstrap_finish_not_recorded",
                       {{"symbol", instr.symbol}, {"run_id", run_id}, {"rows", rows_received},
                        {"fenced", fenced}});
            return;
        }
        log::info("bootstrap_finished", {{"symbol", instr.symbol},
                                         {"status", ok ? "completed" : (aborted ? "aborted" : "failed")},
                                         {"rows", rows_received},
                                         {"requests", request_count},
                                         {"boundary_reached", boundary_reached},
                                         {"actual_from", earliest_seen > 0
                                                             ? timeutil::to_iso8601(earliest_seen)
                                                             : "-"}});
    }

    // ---- reconciliation ------------------------------------------------------
    void reconcile_instrument(const InstrumentRow& instr) {
        const int64_t now = timeutil::now_ms();
        const int64_t to = timeutil::floor_minute(now); // exclusive: current open minute
        const int64_t from = to - reconcile_overlap_min_ * timeutil::kMsPerMinute;
        auto page = adapter_.fetch_candles(instr.symbol, Timeframe::m1, from, to,
                                           exchanges::CandleQueryKind::recent);
        if (!page) {
            if (page.error().code != ErrorCode::shutdown_requested) {
                log::warn("reconcile_fetch_failed", {{"symbol", instr.symbol},
                                                     {"message", page.error().message}});
            }
            return;
        }
        if (page->candles.empty()) return;
        auto pub = publish_candles(instr, std::move(page->candles), CandleSource::rest_reconcile,
                                   "recon", std::to_string(now / 60000));
        if (!pub) {
            log::warn("reconcile_publish_failed", {{"symbol", instr.symbol},
                                                   {"message", pub.error().message}});
        }
    }

    // ---- gap repair -----------------------------------------------------------
    bool handle_repair(const altspotter::v1::Envelope& env) {
        const auto& cmd = env.repair_command();
        InstrumentRow instr{env.instrument_id(), env.exchange_symbol(), 0};
        const int64_t from = cmd.from_ms();
        const int64_t to_incl = cmd.to_ms();
        log::info("repair_started", {{"symbol", instr.symbol},
                                     {"gap_id", cmd.gap_id()},
                                     {"from", timeutil::to_iso8601(from)},
                                     {"to", timeutil::to_iso8601(to_incl)},
                                     {"attempt", cmd.attempt()}});

        int64_t run_id = 0;
        const bool start_recorded = owner_.with_txn("repair_start", [&](pqxx::work& txn) {
            const auto r = txn.exec_params(
                "INSERT INTO ingestion_runs (service, kind, exchange_id, instrument_id, "
                "timeframe, requested_from, requested_to) "
                "VALUES ('market-history-service', 'repair', $1, $2, '1m', "
                "to_timestamp($3::bigint / 1000.0), to_timestamp($4::bigint / 1000.0)) RETURNING id",
                exchange_id_, instr.instrument_id, from, to_incl);
            run_id = r[0][0].as<int64_t>();
        });
        if (!start_recorded) {
            // Do not fetch/publish an unjournalled repair.  The durable command
            // remains available for redelivery and a later safe retry.
            log::error("repair_start_not_recorded",
                       {{"symbol", instr.symbol}, {"gap_id", cmd.gap_id()}});
            return false;
        }

        int64_t rows_received = 0;
        int64_t rows_published = 0;
        int64_t cursor_end = to_incl + timeutil::kMsPerMinute;
        std::string failure;
        while (cursor_end > from && !stop_()) {
            auto page = adapter_.fetch_candles(instr.symbol, Timeframe::m1, from, cursor_end,
                                               exchanges::CandleQueryKind::historical);
            if (!page) {
                failure = page.error().message;
                break;
            }
            log::debug("repair_page", {{"symbol", instr.symbol},
                                       {"gap_id", cmd.gap_id()},
                                       {"from", timeutil::to_iso8601(from)},
                                       {"cursor_end", timeutil::to_iso8601(cursor_end)},
                                       {"rows", static_cast<int64_t>(page->candles.size())}});
            if (page->candles.empty()) break; // exchange has no data for this range
            const int64_t page_earliest = page->candles.front().open_time_ms;
            rows_received += static_cast<int64_t>(page->candles.size());
            auto pub = publish_candles(instr, std::move(page->candles), CandleSource::rest_repair,
                                       "repair", std::to_string(cmd.gap_id()));
            if (!pub) {
                failure = pub.error().message;
                break;
            }
            rows_published += *pub;
            auto next = exchanges::advance_history_cursor(page_earliest, cursor_end);
            if (!next) break;
            cursor_end = *next;
        }

        const int64_t expected =
            (to_incl - from) / timeutil::kMsPerMinute + 1;
        altspotter::v1::RepairStatus status;
        if (!failure.empty()) {
            status = altspotter::v1::REPAIR_STATUS_FAILED;
        } else if (rows_published >= expected) {
            status = altspotter::v1::REPAIR_STATUS_COMPLETED;
        } else if (rows_published > 0) {
            status = altspotter::v1::REPAIR_STATUS_PARTIAL;
        } else {
            status = altspotter::v1::REPAIR_STATUS_PARTIAL; // range confirmed empty on exchange
        }

        bool fenced = false;
        const bool finish_recorded = owner_.with_txn("repair_finish", [&](pqxx::work& txn) {
            fenced = txn.exec_params(history::kFinishRepairRunSql, run_id,
                                     failure.empty() ? "completed" : "failed", failure,
                                     rows_received, rows_published)
                         .affected_rows() == 0;
        });
        if (!finish_recorded || fenced) {
            // The market batches may already be durable in JetStream.  Do not
            // acknowledge the command until its audit row is complete; replay
            // is idempotent and the writer will upsert the same candles. A run
            // this session no longer owns is redone by the owner that does.
            log::error("repair_finish_not_recorded",
                       {{"symbol", instr.symbol},
                        {"gap_id", cmd.gap_id()},
                        {"run_id", run_id},
                        {"fenced", fenced}});
            return false;
        }

        messaging::EnvelopeMeta meta;
        meta.event_type = failure.empty() ? "history.repair.completed" : "history.repair.failed";
        meta.exchange_id = exchange_id_;
        meta.instrument_id = instr.instrument_id;
        meta.exchange_symbol = instr.symbol;
        meta.source_service = "market_history";
        meta.message_id = "rep:" + code_ + ":" + std::to_string(cmd.gap_id()) + ":" +
                          std::to_string(cmd.attempt()) + ":" +
                          (failure.empty() ? "done" : "fail");
        auto out = messaging::make_envelope(meta);
        auto* res = out.mutable_repair_result();
        res->set_gap_id(cmd.gap_id());
        res->set_status(status);
        res->set_timeframe(altspotter::v1::TF_1M);
        res->set_from_ms(from);
        res->set_to_ms(to_incl);
        res->set_rows_received(rows_received);
        res->set_rows_published(rows_published);
        res->set_error(failure);
        const auto subject = subjects::repair_result(code_, instr.symbol, failure.empty());
        if (auto r = nats_.js_publish(subject, messaging::serialize(out), out.message_id());
            !r) {
            log::error("repair_result_publish_failed", {{"message", r.error().message}});
            return false;
        }
        log::info("repair_finished", {{"symbol", instr.symbol},
                                      {"gap_id", cmd.gap_id()},
                                      {"rows", rows_published},
                                      {"status", failure.empty() ? "completed" : "failed"}});
        return true;
    }

    int64_t reconcile_overlap_min() const { return reconcile_overlap_min_; }

    int16_t exchange_id() const { return exchange_id_; }
    const std::string& code() const { return code_; }

private:
    // Sleep up to `d`, waking early to stop. True when stopping.
    bool pause(std::chrono::milliseconds d) {
        const auto until = std::chrono::steady_clock::now() + d;
        while (std::chrono::steady_clock::now() < until) {
            if (stop_()) return true;
            if (ShutdownToken::instance().wait_for(std::chrono::milliseconds(200))) return true;
        }
        return stop_();
    }

    config::ServiceContext& ctx_;
    persistence::PgPool& pool_;
    messaging::NatsClient& nats_;
    exchanges::ExchangeMarketDataAdapter& adapter_;
    history::RunOwner& owner_;
    std::function<bool()> stop_;
    int16_t exchange_id_ = 1;
    std::string code_ = "bitget";
    int64_t bootstrap_days_ = 180;
    size_t publish_batch_size_ = 1000;
    int64_t reconcile_overlap_min_ = 30;
};

// One venue's REST worker set (bootstrap / reconcile / repair each on its own
// single-threaded HttpsClient, so the three run concurrently without racing).
struct VenueHistory {
    int16_t exchange_id;
    std::string code;
    std::unique_ptr<exchanges::ExchangeMarketDataAdapter> bootstrap_adapter;
    std::unique_ptr<exchanges::ExchangeMarketDataAdapter> reconcile_adapter;
    std::unique_ptr<exchanges::ExchangeMarketDataAdapter> repair_adapter;
    std::unique_ptr<HistoryService> bootstrap;
    std::unique_ptr<HistoryService> reconcile;
    std::unique_ptr<HistoryService> repair;
};

} // namespace

namespace altspotter::components {

int market_history(config::ServiceContext ctx) {
    const int64_t reconcile_interval_sec =
        config::opt_int(ctx.service, "reconcile_interval_sec", 300);

    auto& shutdown = ShutdownToken::instance();

    auto nats = messaging::NatsClient::connect(ctx.common.nats_url, ctx.service_name);
    if (!nats) {
        log::critical("nats_unavailable", {{"message", nats.error().message}});
        return 1;
    }
    persistence::PgPool pool(
        ctx.common.postgres_dsn,
        static_cast<size_t>(config::opt_int(ctx.service, "postgres_pool_size", 3)));
    if (auto probe = pool.with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); }); !probe) {
        log::critical("postgres_unavailable", {{"message", probe.error().message}});
        return 1;
    }

    // Only the owner of the runs works (history_runs.hpp). Until this
    // incarnation owns them it waits: no run, no repair command, no venue call.
    // The attempt rides this component's existing one-second loop tick; the
    // tick only sets how soon a standby takes over, never whether a run closes.
    history::RunOwner owner(ctx.common.postgres_dsn);
    bool standby = false;
    while (!shutdown.stop_requested()) {
        const auto taken = owner.take();
        if (taken && *taken) break;
        if (!standby) {
            log::warn("history_standby",
                      {{"message", taken ? std::string("another market_history owner session "
                                                       "holds the runs; waiting for it")
                                         : "run ownership could not be checked: " +
                                               taken.error().message}});
            standby = true;
        }
        if (shutdown.wait_for(std::chrono::seconds(1))) break;
    }
    if (shutdown.stop_requested()) {
        (*nats)->drain(3000);
        log::info("service_stopped", {});
        return 0;
    }
    log::info("history_owner_acquired", {{"after_standby", standby}});

    // Before any run of this incarnation starts, every 'running' row belongs to
    // a session that has ended: close them as a clean shutdown would have.
    auto orphans = owner.close_orphans();
    if (!orphans) {
        log::error("history_orphan_recovery_failed", {{"message", orphans.error().message}});
        return 1;   // the supervisor restarts the component; nothing was started
    }
    for (const auto& o : *orphans) {
        log::warn("history_run_orphaned",
                  {{"run_id", o.id},
                   {"kind", o.kind},
                   {"exchange_id", o.exchange_id},
                   {"instrument_id", o.instrument_id},
                   {"from", o.from_ms > 0 ? timeutil::to_iso8601(o.from_ms) : "-"},
                   {"to", o.to_ms > 0 ? timeutil::to_iso8601(o.to_ms) : "-"},
                   {"started_at", timeutil::to_iso8601(o.started_ms)},
                   {"bootstrap_resumed", o.bootstrap_resumed}});
    }
    log::info("history_orphan_recovery", {{"closed", static_cast<int64_t>(orphans->size())}});

    // Work stops on shutdown and as soon as ownership is lost.
    auto stop_fn = [&shutdown, &owner] { return shutdown.stop_requested() || owner.lost(); };

    // Build one worker set per enabled venue (own adapters + REST config).
    std::vector<std::unique_ptr<VenueHistory>> venues;
    for (const auto& v : exchanges::kVenues) {
        if (!exchanges::venue_enabled(ctx, v.code)) continue;
        const YAML::Node node = exchanges::venue_rest_node(ctx, v.exchange_id);
        auto vh = std::make_unique<VenueHistory>();
        vh->exchange_id = v.exchange_id;
        vh->code = std::string(v.code);
        vh->bootstrap_adapter = exchanges::make_adapter(v.exchange_id, node, stop_fn);
        vh->reconcile_adapter = exchanges::make_adapter(v.exchange_id, node, stop_fn);
        vh->repair_adapter = exchanges::make_adapter(v.exchange_id, node, stop_fn);
        if (!vh->bootstrap_adapter || !vh->reconcile_adapter || !vh->repair_adapter) {
            log::critical("history_venue_config_missing", {{"venue", std::string(v.code)}});
            return 2;
        }
        vh->bootstrap = std::make_unique<HistoryService>(ctx, pool, **nats, *vh->bootstrap_adapter,
                                                         v.exchange_id, std::string(v.code),
                                                         owner, stop_fn);
        vh->reconcile = std::make_unique<HistoryService>(ctx, pool, **nats, *vh->reconcile_adapter,
                                                         v.exchange_id, std::string(v.code),
                                                         owner, stop_fn);
        vh->repair = std::make_unique<HistoryService>(ctx, pool, **nats, *vh->repair_adapter,
                                                      v.exchange_id, std::string(v.code), owner,
                                                      stop_fn);
        venues.push_back(std::move(vh));
    }
    if (venues.empty()) {
        log::critical("history_no_venues", {});
        return 2;
    }

    // Repair worker: one durable consumer on cmd.history.> for all venues;
    // the envelope's exchange selects the venue's repair service.
    std::thread repair_thread([&] {
        auto consumer = (*nats)->pull_consumer("ALTSPOTTER_CONTROL", "history-repair");
        if (!consumer) {
            log::error("repair_consumer_bind_failed", {{"message", consumer.error().message}});
            return;
        }
        while (!stop_fn()) {
            auto msgs = consumer->fetch(4, 2000);
            if (!msgs) {
                log::warn("repair_fetch_failed", {{"message", msgs.error().message}});
                if (shutdown.wait_for(std::chrono::seconds(2))) break;
                continue;
            }
            for (auto& msg : *msgs) {
                auto env = messaging::parse_envelope(msg.data());
                if (!env || !env->has_repair_command()) {
                    log::warn("repair_command_invalid", {{"subject", std::string(msg.subject())}});
                    (void)msg.term();
                    continue;
                }
                const int16_t ex = static_cast<int16_t>(env->exchange());
                HistoryService* svc = nullptr;
                for (auto& vh : venues) {
                    if (vh->exchange_id == (ex > 0 ? ex : 1)) { svc = vh->repair.get(); break; }
                }
                if (svc == nullptr) { (void)msg.term(); continue; }
                // Not handled (or no longer ours to record): redeliver, so the
                // owner of the runs executes it.
                if (!stop_fn() && svc->handle_repair(*env)) (void)msg.ack();
                else (void)msg.nak(1000);
            }
        }
    });

    // Bootstrap worker: all venues, initial pass + periodic re-scan.
    std::thread bootstrap_thread([&] {
        while (!stop_fn()) {
            bool any = false;
            for (auto& vh : venues) {
                if (stop_fn()) break;
                auto instruments = load_active(pool, vh->exchange_id);
                if (!instruments) {
                    log::error("active_load_failed", {{"venue", vh->code},
                                                      {"message", instruments.error().message}});
                    continue;
                }
                if (!instruments->empty()) any = true;
                for (const auto& instr : *instruments) {
                    if (stop_fn()) break;
                    if (vh->bootstrap->needs_bootstrap(instr)) {
                        vh->bootstrap->bootstrap_instrument(instr);
                    }
                }
            }
            const auto delay = any ? std::chrono::seconds(600) : std::chrono::seconds(15);
            for (auto waited = std::chrono::seconds(0); waited < delay && !stop_fn();
                 waited += std::chrono::seconds(1)) {
                if (shutdown.wait_for(std::chrono::seconds(1))) break;
            }
        }
    });

    // Main thread: reconciliation across all venues, and the owner's check on
    // itself once a tick: a session that has ended (a PostgreSQL restart, a
    // terminated backend) ends this incarnation's ownership and its work.
    int64_t next_reconcile = timeutil::now_ms() + 60'000;
    while (!shutdown.wait_for(std::chrono::seconds(1))) {
        if (!owner.holds()) break;
        if (timeutil::now_ms() < next_reconcile) continue;
        next_reconcile = timeutil::now_ms() + reconcile_interval_sec * 1000;
        const int64_t started = timeutil::now_ms();
        int64_t total = 0;
        for (auto& vh : venues) {
            auto instruments = load_active(pool, vh->exchange_id);
            if (!instruments) continue;
            total += static_cast<int64_t>(instruments->size());
            for (const auto& instr : *instruments) {
                if (shutdown.stop_requested() || !owner.holds()) break;
                vh->reconcile->reconcile_instrument(instr);
            }
        }
        log::info("reconcile_cycle_done", {{"instruments", total},
                                           {"venues", venues.size()},
                                           {"duration_ms", timeutil::now_ms() - started}});
    }

    bootstrap_thread.join();
    repair_thread.join();
    (*nats)->drain(3000);
    if (owner.lost() && !shutdown.stop_requested()) {
        // Its runs are fenced and its work has stopped. A fresh incarnation
        // competes for the lock; whichever session wins closes these runs.
        log::error("history_owner_abdicated",
                   {{"message", "run ownership lost; work stopped, the component restarts "
                                "and competes for it again"}});
        return 1;
    }
    log::info("service_stopped", {});
    return 0;
}

}  // namespace altspotter::components
