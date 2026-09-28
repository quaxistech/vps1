// bar-aggregation-service: builds 5m/15m/30m/1h/4h/1d bars from stored 1m
// candles (higher timeframes are built locally, never fetched).
//
// Rules: open = first 1m open, high = max, low = min, close = last 1m close,
// volumes are exact NUMERIC sums; boundaries are strict UTC (date_bin with
// epoch origin). A window missing minutes gets quality_status = 'incomplete'
// and an evt.integrity gap event.
//
// Modes:
//   catch-up : compute every closed window that is complete in 1m but has no
//              complete aggregate. At startup (and for a key this process has
//              not passed yet) back to the aggregate partition floor -- this
//              covers the bootstrap; periodically only the ranges the writer
//              can still have changed: the recent one, and the writable part
//              of each REST backfill since the key's last pass
//              (agg_boundary.hpp). Every pass yields to the live cycle between
//              keys when one is due.
//   live     : each minute, (re)compute the last `recent_recheck_windows`
//              closed windows so late 1m revisions are folded in.
//   repair   : durable consumer on evt.history.> recomputes windows
//              overlapping repaired ranges.
//
// Bars are published to JetStream (bar.bitget.<sym>.<tf>.closed / .agg) and
// persisted by market-storage-writer; recomputation is idempotent (UPSERT).

#include "components/agg_boundary.hpp"
#include "components/components.hpp"
#include "components/history_runs.hpp"
#include "core/readiness.hpp"
#include "core/shutdown.hpp"
#include "config/config.hpp"
#include "logging/logger.hpp"
#include "messaging/codec.hpp"
#include "messaging/nats_client.hpp"
#include "messaging/subjects.hpp"
#include "exchanges/venues.hpp"
#include "persistence/pg.hpp"
#include "core/time.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

using namespace altspotter;

namespace {

struct InstrumentRow {
    int64_t instrument_id = 0;
    std::string symbol;
    int16_t exchange_id = 1;
    std::string code = "bitget";
};

struct AggWindow {
    Candle candle;      // aggregated bar (source = aggregation)
    int64_t minute_count = 0;
    int64_t expected = 0;
    // False when the stored aggregate already carries exactly these values.
    //
    // The live reconciliation pass re-derives a rolling horizon every cycle so
    // a late 1m revision cannot be missed. Announcing the result even when
    // nothing changed is what made a downstream consumer rebuild an unchanged
    // 35-minute window every five minutes, per symbol, indefinitely. A
    // reconciliation exists to publish CHANGES; when there is none there is
    // nothing to say.
    bool changed = true;
};

class AggregationService {
public:
    AggregationService(config::ServiceContext& ctx, persistence::PgPool& pool,
                       messaging::NatsClient& nats)
        : pool_(pool), nats_(nats) {
        const auto tf_node = ctx.service["timeframes"];
        if (tf_node && tf_node.IsSequence()) {
            for (const auto& t : tf_node) {
                if (auto tf = tf_from_label(t.as<std::string>(""))) {
                    if (*tf != Timeframe::m1) timeframes_.push_back(*tf);
                }
            }
        }
        if (timeframes_.empty()) {
            timeframes_ = {Timeframe::m5, Timeframe::m15, Timeframe::m30,
                           Timeframe::h1, Timeframe::h4, Timeframe::d1};
        }
        recheck_windows_ = config::opt_int(ctx.service, "recent_recheck_windows", 3);
        // The live recheck must reach back at least as far as REST
        // reconciliation corrects 1m candles (reconcile_overlap_minutes in
        // market_history, 30 by default); otherwise a late 1m revision in a
        // small timeframe (5m) lands in a window the recheck no longer covers
        // and the stale aggregate would never be recomputed.
        recheck_horizon_min_ = config::opt_int(ctx.service, "recheck_horizon_min", 35);
        publish_batch_size_ =
            static_cast<size_t>(config::opt_int(ctx.service, "publish_batch_size", 500));
        close_delay_sec_ = config::opt_int(ctx.service, "close_delay_sec", 45);
        // How often the periodic catch-up runs. This was a literal ten minutes
        // in the loop; the default keeps exactly that. It is a parameter so a
        // test can run several passes without waiting ten minutes for each.
        catchup_interval_sec_ = config::opt_int(ctx.service, "catchup_interval_sec", 600);
    }

    const std::vector<Timeframe>& timeframes() const { return timeframes_; }
    int64_t close_delay_sec() const { return close_delay_sec_; }
    int64_t recheck_windows() const { return recheck_windows_; }
    int64_t recheck_horizon_ms() const { return recheck_horizon_min_ * 60'000; }
    int64_t catchup_interval_ms() const { return catchup_interval_sec_ * 1000; }

    // The newest 1m minute the writer has committed for this key, or nullopt
    // when it has none. See agg_boundary.hpp for why this bounds a periodic pass.
    Result<std::optional<int64_t>> writer_frontier_ms(const InstrumentRow& instr) {
        std::optional<int64_t> frontier;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto r = txn.exec_params(aggregation::kWriterFrontierSql, instr.exchange_id,
                                           instr.instrument_id);
            if (!r.empty() && !r[0][0].is_null()) frontier = r[0][0].as<int64_t>();
        });
        if (!res) return std::unexpected(res.error());
        return frontier;
    }

    // The REST backfills of this key the writer may not have committed by
    // `committed_through_ms` (agg_boundary.hpp: kBackfillRunsSql).
    Result<std::vector<aggregation::BackfillRun>> backfill_runs(const InstrumentRow& instr,
                                                                int64_t committed_through_ms,
                                                                bool run_owner_alive) {
        std::vector<aggregation::BackfillRun> runs;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            runs.clear();
            const auto rows = txn.exec_params(aggregation::kBackfillRunsSql, instr.exchange_id,
                                              instr.instrument_id, committed_through_ms,
                                              run_owner_alive);
            for (const auto& r : rows) {
                aggregation::BackfillRun run;
                if (!r[0].is_null()) run.from_ms = r[0].as<int64_t>();
                if (!r[1].is_null()) run.to_ms = r[1].as<int64_t>();
                runs.push_back(run);
            }
        });
        if (!res) return std::unexpected(res.error());
        return runs;
    }

    // Whether a market_history incarnation is alive to own 'running' ingestion
    // runs (history_runs.hpp). Read once per pass.
    Result<bool> run_owner_alive() {
        bool alive = false;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            alive = txn.exec_params(history::kRunOwnerAliveSql, history::kRunOwnerLockSpace,
                                    history::kRunOwnerLockKey)[0][0]
                        .as<bool>();
        });
        if (!res) return std::unexpected(res.error());
        return alive;
    }

    int64_t aggregate_floor_ms() const { return agg_floor_ms_.load(std::memory_order_relaxed); }

    // The one authoritative boundary: the lower bound of the oldest surviving
    // market_candles_agg partition. Read from the catalog, so it follows
    // retention drops and partition creation without a second source of truth.
    // On failure the previous value is kept: a transient catalog error must not
    // silently widen (or close) the writable region.
    Result<int64_t> refresh_aggregate_floor() {
        int64_t floor_ms = aggregation::kNoAggregatePartition;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec("SET LOCAL TIME ZONE 'UTC'");
            const auto r = txn.exec(aggregation::kAggregateFloorSql);
            if (!r.empty() && !r[0][0].is_null()) floor_ms = r[0][0].as<int64_t>();
        });
        if (!res) return std::unexpected(res.error());
        agg_floor_ms_.store(floor_ms, std::memory_order_relaxed);
        return floor_ms;
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

    // Aggregate all windows of `tf` whose open_time is in [from_ms, to_ms).
    Result<std::vector<AggWindow>> compute_windows(const InstrumentRow& instr, Timeframe tf,
                                                   int64_t from_ms, int64_t to_ms) {
        std::vector<AggWindow> out;
        const int64_t span = tf_ms(tf);
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec("SET LOCAL TIME ZONE 'UTC'");
            const std::string interval = std::to_string(tf_minutes(tf)) + " minutes";
            const auto rows = txn.exec_params(
                "SELECT (extract(epoch FROM date_bin($3::interval, c.open_time, "
                "        'epoch'::timestamptz)) * 1000)::bigint AS win, "
                "       count(*), "
                "       (array_agg(c.open::text ORDER BY c.open_time))[1], "
                "       max(c.high)::text, min(c.low)::text, "
                "       (array_agg(c.close::text ORDER BY c.open_time DESC))[1], "
                "       sum(c.base_volume)::text, sum(c.quote_volume)::text, "
                "       sum(c.trade_count), "
                "       CASE WHEN bool_and(c.taker_buy_base_volume IS NOT NULL) "
                "            THEN sum(c.taker_buy_base_volume)::text END, "
                "       CASE WHEN bool_and(c.taker_sell_base_volume IS NOT NULL) "
                "            THEN sum(c.taker_sell_base_volume)::text END, "
                // Whether the stored aggregate already says exactly this.
                // Compared as numeric against numeric, and with IS DISTINCT
                // FROM so a nullable column that is null on both sides counts
                // as equal rather than as a change.
                "       (a.open IS NULL "
                "        OR a.open IS DISTINCT FROM (array_agg(c.open ORDER BY c.open_time))[1] "
                "        OR a.high IS DISTINCT FROM max(c.high) "
                "        OR a.low  IS DISTINCT FROM min(c.low) "
                "        OR a.close IS DISTINCT FROM "
                "             (array_agg(c.close ORDER BY c.open_time DESC))[1] "
                "        OR a.base_volume  IS DISTINCT FROM sum(c.base_volume) "
                "        OR a.quote_volume IS DISTINCT FROM sum(c.quote_volume) "
                "        OR a.trade_count  IS DISTINCT FROM sum(c.trade_count) "
                "        OR a.taker_buy_base_volume IS DISTINCT FROM "
                "             (CASE WHEN bool_and(c.taker_buy_base_volume IS NOT NULL) "
                "                   THEN sum(c.taker_buy_base_volume) END) "
                "        OR a.taker_sell_base_volume IS DISTINCT FROM "
                "             (CASE WHEN bool_and(c.taker_sell_base_volume IS NOT NULL) "
                "                   THEN sum(c.taker_sell_base_volume) END) "
                "        OR a.minute_count IS DISTINCT FROM count(*) "
                "        OR a.quality_status IS DISTINCT FROM "
                "             (CASE WHEN count(*) = $6 THEN 'ok' ELSE 'incomplete' END)) "
                "         AS changed "
                "FROM market_candles_1m c "
                "LEFT JOIN market_candles_agg a "
                "  ON a.exchange_id = c.exchange_id AND a.instrument_id = c.instrument_id "
                " AND a.timeframe = $7 "
                " AND a.open_time = date_bin($3::interval, c.open_time, 'epoch'::timestamptz) "
                "WHERE c.exchange_id = $1 AND c.instrument_id = $2 AND c.is_closed "
                "  AND c.open_time >= to_timestamp($4::bigint / 1000.0) "
                "  AND c.open_time < to_timestamp($5::bigint / 1000.0) "
                "GROUP BY win, a.open, a.high, a.low, a.close, a.base_volume, a.quote_volume, "
                "         a.trade_count, a.taker_buy_base_volume, a.taker_sell_base_volume, "
                "         a.minute_count, a.quality_status "
                "ORDER BY win",
                instr.exchange_id, instr.instrument_id, interval, from_ms, to_ms,
                tf_minutes(tf), std::string(tf_label(tf)));

            for (const auto& r : rows) {
                AggWindow w;
                w.candle.timeframe = tf;
                w.candle.open_time_ms = r[0].as<int64_t>();
                w.candle.close_time_ms = w.candle.open_time_ms + span;
                w.minute_count = r[1].as<int64_t>();
                w.expected = span / timeutil::kMsPerMinute;

                const auto dec = [&](int idx) -> Decimal {
                    auto v = Decimal::parse(r[idx].as<std::string>("0"));
                    return v ? *v : Decimal{};
                };
                w.candle.open = dec(2);
                w.candle.high = dec(3);
                w.candle.low = dec(4);
                w.candle.close = dec(5);
                w.candle.base_volume = dec(6);
                w.candle.quote_volume = dec(7);
                if (!r[8].is_null()) w.candle.trade_count = r[8].as<int64_t>();
                if (!r[9].is_null()) w.candle.taker_buy_base_volume = dec(9);
                if (!r[10].is_null()) w.candle.taker_sell_base_volume = dec(10);
                w.candle.is_closed = true;
                w.candle.source = CandleSource::aggregation;
                w.candle.minute_count = w.minute_count;
                w.candle.quality =
                    w.minute_count == w.expected ? Quality::ok : Quality::incomplete;
                w.changed = r[11].is_null() ? true : r[11].as<bool>();
                out.push_back(std::move(w));
            }
        });
        if (!res) return std::unexpected(res.error());
        return out;
    }

    // Earliest CLOSED window that is complete in durable 1m data but has no
    // complete aggregate row.  The repair-result and candle batches live in
    // different JetStream streams, so a repair event may be consumed before
    // storage-writer commits every minute. Treat an existing incomplete/stale
    // aggregate exactly like a missing row; periodic catch-up will repair it
    // once all source minutes are visible instead of preserving the race
    // forever behind an open-time-only watermark.
    //
    // `scan_from_ms` bounds the scan from below: never under the oldest
    // writable aggregate window (see agg_boundary.hpp) -- without that the
    // scan reaches 1m months whose aggregate partition retention already
    // dropped, and every window it finds there is unwritable, which is the
    // poison loop -- and, on a periodic pass, no further back than the writer
    // can still have changed anything.
    Result<int64_t> first_missing_window(const InstrumentRow& instr, Timeframe tf,
                                         int64_t closed_bound_ms, int64_t scan_from_ms) {
        int64_t wm = -1;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec("SET LOCAL TIME ZONE 'UTC'");
            const std::string interval = std::to_string(tf_minutes(tf)) + " minutes";
            const auto r = txn.exec_params(
                aggregation::kFirstMissingWindowSql,
                instr.exchange_id, instr.instrument_id, interval,
                std::string(tf_label(tf)), closed_bound_ms, tf_minutes(tf), scan_from_ms);
            if (!r.empty() && !r[0][0].is_null()) wm = r[0][0].as<int64_t>();
        });
        if (!res) return std::unexpected(res.error());
        return wm;
    }

    Result<int64_t> publish_windows(const InstrumentRow& instr, Timeframe tf,
                                    const std::vector<AggWindow>& all_windows, bool live) {
        if (all_windows.empty()) return 0;

        // The single choke point every path goes through (catch-up, live,
        // repair). A window with no aggregate partition to land in is dropped
        // here rather than handed to the writer, which would reject it and fall
        // into its per-message isolation path. Bounding the catch-up scan keeps
        // this from happening; this keeps it from happening via any other
        // caller, present or future.
        const int64_t floor_ms = aggregate_floor_ms();
        std::vector<AggWindow> writable;
        const std::vector<AggWindow>* selected = &all_windows;
        if (const auto unwritable = static_cast<int64_t>(std::count_if(
                all_windows.begin(), all_windows.end(), [&](const AggWindow& w) {
                    return !aggregation::window_is_writable(w.candle.open_time_ms, floor_ms);
                }));
            unwritable > 0) {
            writable.reserve(all_windows.size() - static_cast<size_t>(unwritable));
            for (const auto& w : all_windows) {
                if (aggregation::window_is_writable(w.candle.open_time_ms, floor_ms)) {
                    writable.push_back(w);
                }
            }
            log::warn("agg_window_below_partition_floor",
                      {{"symbol", instr.symbol},
                       {"timeframe", std::string(tf_label(tf))},
                       {"dropped", unwritable},
                       {"floor", floor_ms == aggregation::kNoAggregatePartition
                                     ? std::string("no aggregate partition")
                                     : timeutil::to_iso8601(floor_ms)}});
            selected = &writable;
        }
        const std::vector<AggWindow>& windows = *selected;
        if (windows.empty()) return 0;

        int64_t published = 0;
        if (live && windows.size() <= 4) {
            for (const auto& w : windows) {
                messaging::EnvelopeMeta meta;
                meta.message_id = messaging::candle_msg_id(instr.code, instr.symbol, tf,
                                                           w.candle.open_time_ms) +
                                  ":r" + std::to_string(w.minute_count);
                meta.event_type = "bar.closed";
                meta.exchange_id = instr.exchange_id;
                meta.instrument_id = instr.instrument_id;
                meta.exchange_symbol = instr.symbol;
                meta.source_service = "bar_aggregation";
                auto env = messaging::make_envelope(meta);
                messaging::to_proto(w.candle, env.mutable_candle());
                auto r = nats_.js_publish(subjects::bar_closed(instr.code, instr.symbol, tf),
                                          messaging::serialize(env), env.message_id());
                if (!r) return std::unexpected(r.error());
                ++published;
                if (w.candle.quality == Quality::incomplete) publish_gap_event(instr, tf, w);
            }
            return published;
        }
        for (size_t off = 0; off < windows.size(); off += publish_batch_size_) {
            const size_t end = std::min(windows.size(), off + publish_batch_size_);
            messaging::EnvelopeMeta meta;
            meta.event_type = "bar.batch.agg";
            meta.exchange_id = instr.exchange_id;
            meta.instrument_id = instr.instrument_id;
            meta.exchange_symbol = instr.symbol;
            meta.source_service = "bar_aggregation";
            meta.message_id = "agg:" + instr.code + ":" + instr.symbol + ":" +
                              std::string(tf_label(tf)) + ":" +
                              std::to_string(windows[off].candle.open_time_ms) + ":" +
                              std::to_string(windows[end - 1].candle.open_time_ms);
            auto env = messaging::make_envelope(meta);
            auto* batch = env.mutable_candle_batch();
            for (size_t i = off; i < end; ++i) {
                messaging::to_proto(windows[i].candle, batch->add_candles());
            }
            auto r = nats_.js_publish(subjects::bar_batch(instr.code, instr.symbol, tf, "agg"),
                                      messaging::serialize(env), env.message_id(), 15000);
            if (!r) return std::unexpected(r.error());
            published += static_cast<int64_t>(end - off);
        }
        return published;
    }

    void publish_gap_event(const InstrumentRow& instr, Timeframe tf, const AggWindow& w) {
        messaging::EnvelopeMeta meta;
        meta.event_type = "integrity.agg_incomplete";
        meta.exchange_id = instr.exchange_id;
        meta.instrument_id = instr.instrument_id;
        meta.exchange_symbol = instr.symbol;
        meta.source_service = "bar_aggregation";
        meta.message_id = "aggincomplete:" + instr.code + ":" + instr.symbol + ":" +
                          std::string(tf_label(tf)) + ":" +
                          std::to_string(w.candle.open_time_ms) + ":" +
                          std::to_string(w.minute_count);
        auto env = messaging::make_envelope(meta);
        auto* ev = env.mutable_integrity_event();
        ev->set_state(altspotter::v1::HEALTH_DEGRADED);
        ev->set_detail("aggregate " + std::string(tf_label(tf)) + " window " +
                       timeutil::to_iso8601(w.candle.open_time_ms) + " has " +
                       std::to_string(w.minute_count) + "/" + std::to_string(w.expected) +
                       " minutes");
        if (auto published = nats_.js_publish(
                subjects::integrity_event(instr.code, instr.symbol, "agg_incomplete"),
                messaging::serialize(env), env.message_id());
            !published) {
            log::warn("agg_incomplete_publish_failed",
                      {{"symbol", instr.symbol}, {"timeframe", std::string(tf_label(tf))},
                       {"message", published.error().message}});
        }
    }

    struct CatchUpResult {
        bool ok = true;        // every timeframe scanned (and published) without error
        int64_t windows = 0;   // windows published by this key's pass
    };

    // Catch-up all timeframes for one instrument over the ranges of its plan
    // (agg_boundary.hpp: plan_catchup), none past the last closed window.
    // `floor_ms` is the aggregate partition floor for this cycle; catch-up is
    // the path that walks backwards through history, so it is the path that
    // must not run without one.
    CatchUpResult catch_up(const InstrumentRow& instr, int64_t now_ms, int64_t floor_ms,
                           const std::vector<aggregation::ScanRange>& ranges) {
        CatchUpResult result;
        if (!aggregation::floor_is_known(floor_ms) ||
            floor_ms == aggregation::kNoAggregatePartition) {
            result.ok = false;
            return result;   // the caller has already logged why
        }
        for (const auto tf : timeframes_) {
            if (ShutdownToken::instance().stop_requested()) {
                result.ok = false;
                return result;
            }
            const int64_t last_closed_window =
                timeutil::floor_interval(now_ms, tf_minutes(tf)); // exclusive bound
            for (const auto& range : aggregation::ranges_for_timeframe(
                     ranges, floor_ms, tf_ms(tf), last_closed_window)) {
                auto wm = first_missing_window(instr, tf, range.to_ms, range.from_ms);
                if (!wm) {
                    log::warn("agg_watermark_failed", {{"symbol", instr.symbol},
                                                       {"message", wm.error().message}});
                    result.ok = false;
                    continue;
                }
                if (*wm < 0) continue; // nothing missing
                if (*wm >= range.to_ms) continue;

                auto windows = compute_windows(instr, tf, *wm, range.to_ms);
                if (!windows) {
                    log::warn("agg_compute_failed", {{"symbol", instr.symbol},
                                                     {"timeframe", std::string(tf_label(tf))},
                                                     {"message", windows.error().message}});
                    result.ok = false;
                    continue;
                }
                if (windows->empty()) continue;
                auto pub = publish_windows(instr, tf, *windows, false);
                if (!pub) {
                    log::error("agg_publish_failed", {{"symbol", instr.symbol},
                                                      {"message", pub.error().message}});
                    result.ok = false;
                    continue;
                }
                result.windows += static_cast<int64_t>(windows->size());
                int64_t incomplete = 0;
                for (const auto& w : *windows) {
                    if (w.candle.quality == Quality::incomplete) ++incomplete;
                }
                log::info("agg_catchup", {{"symbol", instr.symbol},
                                          {"timeframe", std::string(tf_label(tf))},
                                          {"windows", windows->size()},
                                          {"incomplete", incomplete}});
            }
        }
        return result;
    }

    // Live: recompute recent closed windows per timeframe (idempotent).
    void live_cycle(const std::vector<InstrumentRow>& instruments, int64_t now_ms) {
        for (const auto tf : timeframes_) {
            const int64_t last_closed_window = timeutil::floor_interval(now_ms, tf_minutes(tf));
            // At least recheck_windows_ windows, but never a shorter span than
            // the reconcile correction horizon (rounded up to whole windows).
            const int64_t horizon_ms =
                std::max(recheck_windows_ * tf_ms(tf), recheck_horizon_min_ * 60'000);
            const int64_t windows_back = (horizon_ms + tf_ms(tf) - 1) / tf_ms(tf);
            const int64_t from = last_closed_window - windows_back * tf_ms(tf);
            for (const auto& instr : instruments) {
                if (ShutdownToken::instance().stop_requested()) return;
                auto windows = compute_windows(instr, tf, from, last_closed_window);
                if (!windows || windows->empty()) continue;
                // Only what the stored aggregate does not already say. In the
                // steady state that is the one window that just closed, which
                // keeps the publication on the cheap single-bar path instead of
                // the batch path a full horizon would force it onto.
                std::vector<AggWindow> changed;
                changed.reserve(windows->size());
                for (auto& w : *windows) {
                    if (w.changed) changed.push_back(std::move(w));
                }
                if (changed.empty()) continue;
                if (auto pub = publish_windows(instr, tf, changed, true); !pub) {
                    log::warn("agg_live_publish_failed", {{"symbol", instr.symbol},
                                                          {"message", pub.error().message}});
                }
            }
        }
    }

    // Recompute all windows overlapping [from_ms, to_ms] after a repair.
    //
    // The range comes from a repaired 1m gap, and a 1m partition can outlive
    // the aggregate partition of the same month. Repairing history in that band
    // is legitimate for 1m and impossible for the aggregate, so the range is
    // clamped to the writable region before any window is computed.
    void recompute_range(const InstrumentRow& instr, int64_t from_ms, int64_t to_ms) {
        if (auto floor = refresh_aggregate_floor(); !floor) {
            log::warn("agg_floor_refresh_failed", {{"symbol", instr.symbol},
                                                   {"message", floor.error().message}});
        }
        const auto writable =
            aggregation::clamp_to_writable(from_ms, to_ms, aggregate_floor_ms());
        if (!writable) {
            log::info("agg_recompute_below_partition_floor",
                      {{"symbol", instr.symbol},
                       {"from", timeutil::to_iso8601(from_ms)},
                       {"to", timeutil::to_iso8601(to_ms)}});
            return;
        }
        from_ms = writable->first;
        to_ms = writable->second;
        for (const auto tf : timeframes_) {
            const int64_t start = timeutil::floor_interval(from_ms, tf_minutes(tf));
            const int64_t end = timeutil::floor_interval(to_ms, tf_minutes(tf)) + tf_ms(tf);
            auto windows = compute_windows(instr, tf, start, end);
            if (!windows || windows->empty()) continue;
            if (auto pub = publish_windows(instr, tf, *windows, false); !pub) {
                log::warn("agg_recompute_publish_failed", {{"symbol", instr.symbol},
                                                           {"message", pub.error().message}});
            }
        }
        log::info("agg_recomputed_after_repair", {{"symbol", instr.symbol},
                                                  {"from", timeutil::to_iso8601(from_ms)},
                                                  {"to", timeutil::to_iso8601(to_ms)}});
    }

private:
    persistence::PgPool& pool_;
    messaging::NatsClient& nats_;
    std::vector<Timeframe> timeframes_;
    int64_t recheck_windows_ = 3;
    int64_t recheck_horizon_min_ = 35;
    size_t publish_batch_size_ = 500;
    int64_t close_delay_sec_ = 45;
    int64_t catchup_interval_sec_ = 600;
    // Read by the main loop and by the repair thread; written by whichever
    // refreshes it. Relaxed is enough: a stale read costs one cycle, and the
    // guard it feeds is conservative in both directions.
    std::atomic<int64_t> agg_floor_ms_{aggregation::kFloorUnknown};
};

} // namespace

namespace altspotter::components {

int bar_aggregation(config::ServiceContext ctx) {
    const int64_t cycle_interval_sec = config::opt_int(ctx.service, "cycle_interval_sec", 60);

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

    AggregationService service(ctx, pool, **nats);
    auto& shutdown = ShutdownToken::instance();

    // Repair-event consumer: recompute affected windows after gap repair.
    std::thread repair_thread([&] {
        auto consumer = (*nats)->pull_consumer("ALTSPOTTER_CONTROL", "aggregation-repair-events");
        if (!consumer) {
            log::error("agg_repair_consumer_failed", {{"message", consumer.error().message}});
            return;
        }
        while (!shutdown.stop_requested()) {
            auto msgs = consumer->fetch(8, 2000);
            if (!msgs) {
                if (shutdown.wait_for(std::chrono::seconds(2))) break;
                continue;
            }
            for (auto& msg : *msgs) {
                auto env = messaging::parse_envelope(msg.data());
                if (env && env->has_repair_result() &&
                    env->repair_result().status() != altspotter::v1::REPAIR_STATUS_FAILED &&
                    env->repair_result().rows_published() > 0) {
                    const int16_t ex = static_cast<int16_t>(env->exchange());
                    InstrumentRow instr{env->instrument_id(), env->exchange_symbol(),
                                        ex > 0 ? ex : int16_t{1},
                                        std::string(exchanges::venue_code(ex > 0 ? ex : 1))};
                    service.recompute_range(instr, env->repair_result().from_ms(),
                                            env->repair_result().to_ms());
                }
                (void)msg.ack();
            }
        }
    });

    // Startup catch-up (covers bootstrap backlog), then per-minute live cycle
    // with periodic catch-up in case the writer lags behind.
    //
    // The first pass reaches back to the partition floor for every key. Later
    // passes scan only the ranges the writer can still have changed
    // (agg_boundary.hpp: plan_catchup), which is what keeps them short. And a pass, however long, yields to the live cycle between
    // keys whenever a live cycle is due: catch-up and live share this one
    // loop, and a pass that held it for minutes is what published 15m bars up
    // to ten minutes late.
    int64_t last_catchup = 0;
    int64_t last_live = 0;
    bool first_pass = true;
    // Writer frontier per key at the start of its last successful pass.
    std::map<std::pair<int16_t, int64_t>, int64_t> verified_frontier;
    // Live cycles run from inside a pass, and the time they took, so the pass
    // log can say how long catch-up itself held the loop.
    int64_t pass_live_cycles = 0;
    int64_t pass_live_ms = 0;
    const auto live_if_due = [&](const std::vector<InstrumentRow>& instruments) {
        const int64_t now = timeutil::now_ms();
        if (now - last_live < cycle_interval_sec * 1000) return;
        service.live_cycle(instruments, now - service.close_delay_sec() * 1000);
        last_live = timeutil::now_ms();
        ++pass_live_cycles;
        pass_live_ms += last_live - now;
    };
    while (!shutdown.stop_requested()) {
        auto instruments = service.load_active();
        if (!instruments) {
            log::error("agg_active_load_failed", {{"message", instruments.error().message}});
            if (shutdown.wait_for(std::chrono::seconds(10))) break;
            continue;
        }
        // Aggregate only data that closed before (now - close_delay): gives the
        // writer time to flush the final 1m candles of the window.
        const int64_t effective_now = timeutil::now_ms() - service.close_delay_sec() * 1000;

        if (timeutil::now_ms() - last_catchup >= service.catchup_interval_ms()) {
            // One catalog read per cycle, not per instrument: the aggregate
            // partition floor moves only when retention drops a partition or
            // maintenance creates one.
            const auto floor = service.refresh_aggregate_floor();
            if (!floor) {
                log::warn("agg_floor_refresh_failed", {{"message", floor.error().message}});
            } else if (*floor == aggregation::kNoAggregatePartition) {
                log::error("agg_no_partition_space",
                           {{"message", "market_candles_agg has no range partitions; "
                                        "catch-up is suspended until maintenance creates them"}});
            }
            const int64_t floor_ms = service.aggregate_floor_ms();
            const int64_t pass_started = timeutil::now_ms();
            pass_live_cycles = 0;
            pass_live_ms = 0;
            const int64_t horizon_ms = service.recheck_horizon_ms();
            int64_t keys = 0, deep_keys = 0, windows = 0, failed = 0;
            int64_t backfill_ranges = 0, runs_below_floor = 0, runs_clamped = 0;
            int64_t widest_span_ms = 0, slowest_key_ms = 0;
            std::string slowest_key = "-";
            int64_t oldest_scan_from = std::numeric_limits<int64_t>::max();
            const bool floor_usable = aggregation::floor_is_known(floor_ms) &&
                                      floor_ms != aggregation::kNoAggregatePartition;
            // Unknown counts as alive: a 'running' row is then rescanned, which
            // costs one pass some time and never loses a window.
            bool owner_alive = true;
            if (auto alive = service.run_owner_alive(); alive) {
                owner_alive = *alive;
            } else {
                log::warn("agg_run_owner_check_failed", {{"message", alive.error().message}});
            }
            for (const auto& instr : *instruments) {
                if (!floor_usable || shutdown.stop_requested()) break;
                const auto key = std::make_pair(instr.exchange_id, instr.instrument_id);
                // Observed BEFORE the scan: whatever the writer commits while
                // this key is being scanned is past it, so the next pass sees it.
                auto frontier = service.writer_frontier_ms(instr);
                if (!frontier) {
                    log::warn("agg_frontier_failed", {{"symbol", instr.symbol},
                                                      {"message", frontier.error().message}});
                    ++failed;
                    continue;
                }
                std::optional<int64_t> prev;
                if (const auto it = verified_frontier.find(key); it != verified_frontier.end()) {
                    prev = it->second;
                }
                std::vector<aggregation::BackfillRun> runs;
                if (prev) {
                    auto found = service.backfill_runs(instr, *prev - horizon_ms, owner_alive);
                    if (!found) {
                        log::warn("agg_backfill_runs_failed",
                                  {{"symbol", instr.symbol}, {"message", found.error().message}});
                        ++failed;
                        continue;
                    }
                    runs = std::move(*found);
                }
                const auto plan = aggregation::plan_catchup(floor_ms, prev, horizon_ms, runs);
                if (plan.full_reach(floor_ms)) ++deep_keys;
                backfill_ranges += static_cast<int64_t>(runs.size()) - plan.runs_below_floor;
                runs_below_floor += plan.runs_below_floor;
                runs_clamped += plan.runs_clamped;
                widest_span_ms =
                    std::max(widest_span_ms, aggregation::plan_span_ms(plan.ranges, effective_now));
                oldest_scan_from = std::min(oldest_scan_from, plan.ranges.front().from_ms);
                const int64_t key_started = timeutil::now_ms();
                const auto result = service.catch_up(instr, effective_now, floor_ms, plan.ranges);
                if (const int64_t took = timeutil::now_ms() - key_started; took > slowest_key_ms) {
                    slowest_key_ms = took;
                    slowest_key = instr.symbol;
                }
                ++keys;
                windows += result.windows;
                if (!result.ok) {
                    ++failed;   // the frontier stays where it was: the next pass retries
                } else if (*frontier) {
                    verified_frontier[key] = **frontier;
                }
                live_if_due(*instruments);
            }
            log::info("agg_catchup_pass",
                      {{"mode", std::string(first_pass ? "startup" : "periodic")},
                       {"keys", keys},
                       {"deep_keys", deep_keys},
                       {"windows", windows},
                       {"failed", failed},
                       {"duration_ms", timeutil::now_ms() - pass_started},
                       {"catchup_ms", timeutil::now_ms() - pass_started - pass_live_ms},
                       {"live_cycles", pass_live_cycles},
                       {"backfill_ranges", backfill_ranges},
                       {"runs_below_floor", runs_below_floor},
                       {"runs_clamped", runs_clamped},
                       {"widest_span_min", widest_span_ms / 60'000},
                       {"slowest_key_ms", slowest_key_ms},
                       {"slowest_key", slowest_key},
                       {"run_owner_alive", owner_alive},
                       {"oldest_scan_from",
                        oldest_scan_from == std::numeric_limits<int64_t>::max()
                            ? std::string("none")
                            : timeutil::to_iso8601(oldest_scan_from)}});
            first_pass = false;
            last_catchup = timeutil::now_ms();
        } else {
            service.live_cycle(*instruments, effective_now);
            last_live = timeutil::now_ms();
        }
        if (shutdown.wait_for(std::chrono::seconds(cycle_interval_sec))) break;
    }

    repair_thread.join();
    (*nats)->drain(3000);
    log::info("service_stopped", {});
    return 0;
}

}  // namespace altspotter::components
