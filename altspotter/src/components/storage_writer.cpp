// market-storage-writer: the only component that writes candles to PostgreSQL.
//
// Durable JetStream pull consumer on ALTSPOTTER_BARS (bar.>):
//   * single closed candles (Envelope.candle) from the live gateway and
//     bar-aggregation-service;
//   * CandleBatch messages from market-history-service (backfill / reconcile /
//     repair) and aggregation catch-up.
//
// Guarantees:
//   * validation before write (schema version, timestamps, OHLC, volumes);
//   * batch UPSERT; identical duplicates are no-ops, changed values bump
//     `revision` (audited via source/updated_at);
//   * ACK only after successful commit — PostgreSQL outage causes redelivery,
//     never data loss; bounded fetch = natural backpressure;
//   * invalid events are journalled in rejected_market_events; a message that
//     fails with a non-retryable SQL error is isolated per message so one
//     poison message can never block the stream (recorded + TERMed).

#include "components/components.hpp"
#include "core/backoff.hpp"
#include "core/readiness.hpp"
#include "core/shutdown.hpp"
#include "config/config.hpp"
#include "logging/logger.hpp"
#include "messaging/codec.hpp"
#include "messaging/nats_client.hpp"
#include "persistence/candle_store.hpp"
#include "persistence/pg.hpp"
#include "core/time.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string_view>

using namespace altspotter;

namespace {

struct Rejected {
    std::string subject;
    std::string reason;
    std::string error;
    std::string payload;
};

struct ParsedMsg {
    std::vector<persistence::CandleRow> rows_1m;
    std::vector<persistence::CandleRow> rows_agg;
    std::optional<Rejected> rejected;
};

ParsedMsg parse_message(const messaging::Msg& msg) {
    ParsedMsg out;
    auto env = messaging::parse_envelope(msg.data());
    if (!env) {
        out.rejected = Rejected{std::string(msg.subject()), "envelope_parse",
                                env.error().message, std::string(msg.data())};
        return out;
    }

    const int64_t now_ms = timeutil::now_ms();
    const auto add_candle = [&](const altspotter::v1::Candle& pc) {
        auto candle = messaging::candle_from_proto(pc);
        if (!candle) {
            out.rejected = Rejected{std::string(msg.subject()), "candle_decode",
                                    candle.error().message, std::string(msg.data())};
            return;
        }
        if (!candle->is_closed) {
            out.rejected = Rejected{std::string(msg.subject()), "open_candle",
                                    "refusing to persist a non-closed candle",
                                    std::string(msg.data())};
            return;
        }
        if (!candle->valid()) {
            out.rejected = Rejected{std::string(msg.subject()), "candle_invalid",
                                    "OHLC/timestamp validation failed", std::string(msg.data())};
            return;
        }
        // A "closed" candle cannot open in the future (small skew allowance).
        // Far-future timestamps would miss every partition and poison writes.
        if (candle->open_time_ms > now_ms + 2 * timeutil::kMsPerMinute) {
            out.rejected = Rejected{std::string(msg.subject()), "future_candle",
                                    "closed candle with open_time in the future",
                                    std::string(msg.data())};
            return;
        }
        if (env->instrument_id() <= 0) {
            out.rejected = Rejected{std::string(msg.subject()), "missing_instrument",
                                    "envelope has no instrument_id", std::string(msg.data())};
            return;
        }
        // Venue from the envelope (1 bitget, 2 binance, 3 okx, 4 bybit).
        const int16_t ex = static_cast<int16_t>(env->exchange());
        if (ex <= 0) {
            out.rejected = Rejected{std::string(msg.subject()), "missing_exchange",
                                    "envelope has no exchange", std::string(msg.data())};
            return;
        }
        persistence::CandleRow row;
        row.exchange_id = ex;
        row.instrument_id = env->instrument_id();
        row.candle = std::move(*candle);
        row.exchange_received_at_ms = env->exchange_timestamp_ms();
        if (row.candle.timeframe == Timeframe::m1) {
            out.rows_1m.push_back(std::move(row));
        } else {
            out.rows_agg.push_back(std::move(row));
        }
    };

    if (env->has_candle()) {
        add_candle(env->candle());
    } else if (env->has_candle_batch()) {
        for (const auto& pc : env->candle_batch().candles()) {
            add_candle(pc);
            if (out.rejected) break; // one bad row poisons the message, keep it atomic
        }
        if (out.rejected) {
            out.rows_1m.clear();
            out.rows_agg.clear();
        }
    } else {
        out.rejected = Rejected{std::string(msg.subject()), "unexpected_payload",
                                "envelope carries no candle payload", std::string(msg.data())};
    }
    return out;
}

void insert_rejected(pqxx::work& txn, const Rejected& rej) {
    txn.exec("INSERT INTO rejected_market_events (subject, reason, error, payload) "
             "VALUES (" + txn.quote(rej.subject) + ", " + txn.quote(rej.reason) + ", " +
             txn.quote(rej.error) + ", " +
             txn.quote(std::basic_string_view<std::byte>{
                 reinterpret_cast<const std::byte*>(rej.payload.data()),
                 rej.payload.size()}) + ")");
}

// One transaction: chunked UPSERTs for both candle tables + rejected records.
Result<std::pair<persistence::WriteStats, persistence::WriteStats>> write_rows(
    persistence::PgPool& pool, size_t batch_size,
    std::vector<persistence::CandleRow> rows_1m, std::vector<persistence::CandleRow> rows_agg,
    const std::vector<Rejected>& rejected) {
    persistence::dedupe_candle_rows(rows_1m, false);
    persistence::dedupe_candle_rows(rows_agg, true);
    persistence::WriteStats delta_1m{}, delta_agg{};
    auto res = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        for (size_t off = 0; off < rows_1m.size(); off += batch_size) {
            const auto end = std::min(rows_1m.size(), off + batch_size);
            std::vector<persistence::CandleRow> chunk(rows_1m.begin() + off,
                                                      rows_1m.begin() + end);
            auto st = persistence::upsert_candles_1m(txn, chunk);
            if (!st) throw pqxx::usage_error(st.error().message);
            delta_1m.inserted += st->inserted;
            delta_1m.updated += st->updated;
            delta_1m.identical += st->identical;
        }
        for (size_t off = 0; off < rows_agg.size(); off += batch_size) {
            const auto end = std::min(rows_agg.size(), off + batch_size);
            std::vector<persistence::CandleRow> chunk(rows_agg.begin() + off,
                                                      rows_agg.begin() + end);
            auto st = persistence::upsert_candles_agg(txn, chunk);
            if (!st) throw pqxx::usage_error(st.error().message);
            delta_agg.inserted += st->inserted;
            delta_agg.updated += st->updated;
            delta_agg.identical += st->identical;
        }
        for (const auto& rej : rejected) insert_rejected(txn, rej);
    });
    if (!res) return std::unexpected(res.error());
    return std::pair{delta_1m, delta_agg};
}

} // namespace

namespace altspotter::components {

int storage_writer(config::ServiceContext ctx) {

    const auto consumer_node = ctx.service["consumer"];
    const std::string stream = config::opt_str(consumer_node, "stream", "ALTSPOTTER_BARS");
    const std::string durable = config::opt_str(consumer_node, "durable", "storage-writer-bars");
    const int fetch_batch = static_cast<int>(config::opt_int(consumer_node, "fetch_batch", 256));
    const int64_t fetch_timeout_ms = config::opt_int(consumer_node, "fetch_timeout_ms", 1500);
    const size_t batch_size = static_cast<size_t>(config::opt_int(ctx.service, "batch_size", 2000));
    const int64_t db_backoff_min = config::opt_int(ctx.service, "db_retry_backoff_min_ms", 500);
    const int64_t db_backoff_max = config::opt_int(ctx.service, "db_retry_backoff_max_ms", 30000);

    auto nats = messaging::NatsClient::connect(ctx.common.nats_url, ctx.service_name);
    if (!nats) {
        log::critical("nats_unavailable", {{"message", nats.error().message}});
        return 1;
    }
    auto consumer = (*nats)->pull_consumer(stream, durable);
    if (!consumer) {
        log::critical("consumer_bind_failed", {{"message", consumer.error().message},
                                               {"stream", stream},
                                               {"durable", durable}});
        return 1;
    }

    persistence::PgPool pool(
        ctx.common.postgres_dsn,
        static_cast<size_t>(config::opt_int(ctx.service, "postgres_pool_size", 4)));
    if (auto probe = pool.with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); }); !probe) {
        log::critical("postgres_unavailable", {{"message", probe.error().message}});
        return 1;
    }

    auto& shutdown = ShutdownToken::instance();

    Backoff db_backoff{std::chrono::milliseconds(db_backoff_min),
                       std::chrono::milliseconds(db_backoff_max)};
    int64_t stats_last_ms = timeutil::now_ms();
    persistence::WriteStats stats_1m{}, stats_agg{};
    int64_t stats_rejected = 0;

    while (!shutdown.stop_requested()) {
        auto msgs = consumer->fetch(fetch_batch, fetch_timeout_ms);
        if (!msgs) {
            log::warn("fetch_failed", {{"message", msgs.error().message}});
            if (shutdown.wait_for(std::chrono::seconds(1))) break;
            continue;
        }
        if (msgs->empty()) {
            // periodic stats on idle
            const int64_t now = timeutil::now_ms();
            if (now - stats_last_ms >= 60000) {
                log::info("writer_stats", {{"inserted_1m", stats_1m.inserted},
                                           {"updated_1m", stats_1m.updated},
                                           {"identical_1m", stats_1m.identical},
                                           {"inserted_agg", stats_agg.inserted},
                                           {"updated_agg", stats_agg.updated},
                                           {"rejected", stats_rejected}});
                stats_1m = {};
                stats_agg = {};
                stats_rejected = 0;
                stats_last_ms = now;
            }
            continue;
        }

        std::vector<ParsedMsg> parsed;
        parsed.reserve(msgs->size());
        std::vector<persistence::CandleRow> rows_1m, rows_agg;
        std::vector<Rejected> rejected;
        for (const auto& msg : *msgs) {
            parsed.push_back(parse_message(msg));
            const auto& p = parsed.back();
            if (p.rejected) rejected.push_back(*p.rejected);
            for (const auto& r : p.rows_1m) rows_1m.push_back(r);
            for (const auto& r : p.rows_agg) rows_agg.push_back(r);
        }

        // Write everything from this fetch in one transaction (chunked statements).
        auto batch = write_rows(pool, batch_size, std::move(rows_1m), std::move(rows_agg),
                                rejected);
        if (batch) {
            db_backoff.reset();
            const auto& [delta_1m, delta_agg] = *batch;
            stats_1m.inserted += delta_1m.inserted;
            stats_1m.updated += delta_1m.updated;
            stats_1m.identical += delta_1m.identical;
            stats_agg.inserted += delta_agg.inserted;
            stats_agg.updated += delta_agg.updated;
            stats_agg.identical += delta_agg.identical;
            stats_rejected += static_cast<int64_t>(rejected.size());
            for (auto& msg : *msgs) {
                if (auto a = msg.ack(); !a) {
                    log::warn("ack_failed", {{"message", a.error().message}});
                }
            }
            if (delta_1m.updated > 0 || delta_agg.updated > 0) {
                log::info("writer_revisions", {{"updated_1m", delta_1m.updated},
                                               {"updated_agg", delta_agg.updated}});
            }
        } else if (batch.error().retryable) {
            // DB outage/deadlock: no premature ACK — NAK with delay and back off.
            const auto delay = db_backoff.next();
            log::error("writer_db_unavailable",
                       {{"message", batch.error().message},
                        {"retry_count", db_backoff.attempt()},
                        {"duration_ms", delay.count()}});
            for (auto& msg : *msgs) {
                (void)msg.nak(delay.count());
            }
            if (shutdown.wait_for(delay)) break;
        } else {
            // Non-retryable SQL failure: one poison message must not block the
            // stream forever (MaxDeliver is unlimited). Isolate per message:
            // good messages commit and ACK; the poison one is recorded in
            // rejected_market_events and TERMed so the consumer advances.
            log::error("writer_batch_failed_isolating",
                       {{"message", batch.error().message},
                        {"batch", msgs->size()}});
            for (size_t i = 0; i < msgs->size(); ++i) {
                auto& msg = (*msgs)[i];
                const auto& p = parsed[i];
                std::vector<Rejected> rej_one;
                if (p.rejected) rej_one.push_back(*p.rejected);
                auto one = write_rows(pool, batch_size, p.rows_1m, p.rows_agg, rej_one);
                if (one) {
                    stats_1m.inserted += one->first.inserted;
                    stats_1m.updated += one->first.updated;
                    stats_1m.identical += one->first.identical;
                    stats_agg.inserted += one->second.inserted;
                    stats_agg.updated += one->second.updated;
                    stats_agg.identical += one->second.identical;
                    stats_rejected += static_cast<int64_t>(rej_one.size());
                    (void)msg.ack();
                } else if (one.error().retryable) {
                    (void)msg.nak(2000);
                } else {
                    const Rejected poison{std::string(msg.subject()), "write_failed",
                                          one.error().message, std::string(msg.data())};
                    auto rec = pool.with_txn([&](pqxx::work& txn) {
                        insert_rejected(txn, poison);
                    });
                    if (rec) {
                        ++stats_rejected;
                        // Explicitly surface retention/partition conflicts:
                        // the payload is retained in rejected_market_events,
                        // NOT silently treated as a restored market candle.
                        if (poison.error.find("no partition of relation") != std::string::npos) {
                            log::error("writer_missing_partition_quarantined",
                                       {{"subject", poison.subject},
                                        {"message", poison.error}});
                        }
                        log::error("writer_poison_message_rejected",
                                   {{"subject", poison.subject},
                                    {"message", poison.error}});
                        (void)msg.term();
                    } else {
                        // Could not even journal it: keep it redelivering rather
                        // than dropping data silently.
                        (void)msg.nak(5000);
                    }
                }
            }
        }
    }

    (*nats)->drain(3000);
    log::info("service_stopped", {});
    return 0;
}

}  // namespace altspotter::components
