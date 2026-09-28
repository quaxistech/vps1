// microstructure-aggregation: rolls the high-volume raw microstructure tables
// into compact 1s/1m aggregates BEFORE retention drops the raw partitions.
//
//   market_trades_raw            -> market_trade_aggregates_1s / _1m
//   market_orderbook_snapshots   -> market_book_aggregates_1s  / _1m
//
// Incremental + idempotent: each (exchange_id, instrument_id, table) advances
// from the aggregate watermark (max open_time) minus a small overlap up to
// now - lag, UPSERTing every window. Retention never removes raw rows whose
// compact aggregates do not yet exist because the aggregator keeps the
// watermark current (retention checks the 1s/1m coverage before dropping raw).

#include "components/components.hpp"
#include "core/readiness.hpp"
#include "core/shutdown.hpp"
#include "config/config.hpp"
#include "logging/logger.hpp"
#include "persistence/pg.hpp"
#include "persistence/txn_log.hpp"
#include "core/time.hpp"

#include <string>
#include <vector>

using namespace altspotter;

namespace {

struct Inst {
    int16_t exchange_id;
    int64_t instrument_id;
    int retention_class; // 1 core, 0 other
};

Result<std::vector<Inst>> load_active(persistence::PgPool& pool) {
    std::vector<Inst> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec(
            "SELECT ei.exchange_id, ei.instrument_id, "
            "       (rca.base_asset IS NOT NULL)::int "
            "FROM exchange_instruments ei "
            "JOIN instruments i ON i.id = ei.instrument_id "
            "LEFT JOIN retention_core_assets rca ON rca.base_asset = i.base_asset "
            "WHERE ei.is_whitelisted AND ei.status = 'active'");
        for (const auto& r : rows) {
            out.push_back({r[0].as<int16_t>(), r[1].as<int64_t>(), r[2].as<int>()});
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

// Watermark (max open_time ms) of an aggregate table for one key.
int64_t watermark(pqxx::work& txn, const std::string& table, int16_t ex, int64_t iid,
                  const char* class_pred) {
    const auto r = txn.exec_params(
        "SELECT COALESCE((extract(epoch FROM max(open_time)) * 1000)::bigint, 0) FROM " + table +
            " WHERE exchange_id = $1 AND instrument_id = $2" + class_pred,
        ex, iid);
    return r[0][0].as<int64_t>();
}

class Aggregator {
public:
    Aggregator(persistence::PgPool& pool, config::ServiceContext& ctx) : pool_(pool) {
        lag_sec_ = config::opt_int(ctx.service, "aggregation_lag_sec", 5);
        overlap_sec_ = config::opt_int(ctx.service, "aggregation_overlap_sec", 120);
        max_window_min_ = config::opt_int(ctx.service, "max_window_minutes", 240);
    }

    void run_cycle(const std::vector<Inst>& instruments) {
        const int64_t now = timeutil::now_ms();
        const int64_t to = now - lag_sec_ * 1000;
        for (const auto& in : instruments) {
            if (ShutdownToken::instance().stop_requested()) return;
            roll_trades_1s(in, to);
            roll_trades_1m(in, to);
            roll_books_1s(in, to);
            roll_books_1m(in, to);
        }
    }

private:
    int64_t start_from(pqxx::work& txn, const std::string& table, const Inst& in,
                       const char* class_pred, int64_t to) {
        int64_t wm = watermark(txn, table, in.exchange_id, in.instrument_id, class_pred);
        int64_t from = wm > 0 ? wm - overlap_sec_ * 1000 : to - 3600'000; // 1h cold start
        const int64_t max_span = max_window_min_ * 60'000;
        if (to - from > max_span) from = to - max_span;
        return from;
    }

    void roll_trades_1s(const Inst& in, int64_t to) {
        persistence::with_txn_logged(pool_, "agg_trades_1s", [&](pqxx::work& txn) {
            const int64_t from = start_from(txn, "market_trade_aggregates_1s", in,
                                            in.retention_class == 1 ? " AND retention_class = 1"
                                                                    : " AND retention_class = 0",
                                            to);
            txn.exec_params(
                "INSERT INTO market_trade_aggregates_1s (retention_class, exchange_id, "
                "instrument_id, open_time, trade_count, buy_trade_count, sell_trade_count, "
                "base_volume, quote_volume, aggressive_buy_volume, aggressive_sell_volume, "
                "signed_volume, vwap, min_price, max_price, first_price, last_price, "
                "largest_trade, quality_status) "
                "SELECT $3, $1, $2, date_trunc('second', trade_time), count(*), "
                "count(*) FILTER (WHERE side='buy'), count(*) FILTER (WHERE side='sell'), "
                "sum(size), sum(size*price), "
                "COALESCE(sum(size) FILTER (WHERE side='buy'),0), "
                "COALESCE(sum(size) FILTER (WHERE side='sell'),0), "
                "sum(CASE WHEN side='buy' THEN size ELSE -size END), "
                "CASE WHEN sum(size)>0 THEN sum(size*price)/sum(size) END, "
                "min(price), max(price), "
                "(array_agg(price ORDER BY trade_time, trade_id))[1], "
                "(array_agg(price ORDER BY trade_time DESC, trade_id DESC))[1], "
                "max(size), 'ok' "
                "FROM market_trades_raw "
                "WHERE exchange_id=$1 AND instrument_id=$2 "
                "  AND trade_time >= to_timestamp($4::bigint/1000.0) "
                "  AND trade_time <  to_timestamp($5::bigint/1000.0) "
                "GROUP BY date_trunc('second', trade_time) "
                "ON CONFLICT (retention_class, exchange_id, instrument_id, open_time) DO UPDATE SET "
                "trade_count=EXCLUDED.trade_count, buy_trade_count=EXCLUDED.buy_trade_count, "
                "sell_trade_count=EXCLUDED.sell_trade_count, base_volume=EXCLUDED.base_volume, "
                "quote_volume=EXCLUDED.quote_volume, "
                "aggressive_buy_volume=EXCLUDED.aggressive_buy_volume, "
                "aggressive_sell_volume=EXCLUDED.aggressive_sell_volume, "
                "signed_volume=EXCLUDED.signed_volume, vwap=EXCLUDED.vwap, "
                "min_price=EXCLUDED.min_price, max_price=EXCLUDED.max_price, "
                "first_price=EXCLUDED.first_price, last_price=EXCLUDED.last_price, "
                "largest_trade=EXCLUDED.largest_trade",
                in.exchange_id, in.instrument_id, in.retention_class, from, to);
        });
    }

    void roll_trades_1m(const Inst& in, int64_t to) {
        persistence::with_txn_logged(pool_, "agg_trades_1m", [&](pqxx::work& txn) {
            const int64_t from = start_from(txn, "market_trade_aggregates_1m", in, "", to);
            txn.exec_params(
                "INSERT INTO market_trade_aggregates_1m (exchange_id, instrument_id, open_time, "
                "trade_count, buy_trade_count, sell_trade_count, base_volume, quote_volume, "
                "aggressive_buy_volume, aggressive_sell_volume, signed_volume, vwap, min_price, "
                "max_price, first_price, last_price, largest_trade, quality_status) "
                "SELECT $1, $2, date_trunc('minute', trade_time), count(*), "
                "count(*) FILTER (WHERE side='buy'), count(*) FILTER (WHERE side='sell'), "
                "sum(size), sum(size*price), "
                "COALESCE(sum(size) FILTER (WHERE side='buy'),0), "
                "COALESCE(sum(size) FILTER (WHERE side='sell'),0), "
                "sum(CASE WHEN side='buy' THEN size ELSE -size END), "
                "CASE WHEN sum(size)>0 THEN sum(size*price)/sum(size) END, "
                "min(price), max(price), "
                "(array_agg(price ORDER BY trade_time, trade_id))[1], "
                "(array_agg(price ORDER BY trade_time DESC, trade_id DESC))[1], "
                "max(size), 'ok' "
                "FROM market_trades_raw "
                "WHERE exchange_id=$1 AND instrument_id=$2 "
                "  AND trade_time >= to_timestamp($3::bigint/1000.0) "
                "  AND trade_time <  to_timestamp($4::bigint/1000.0) "
                "GROUP BY date_trunc('minute', trade_time) "
                "ON CONFLICT (exchange_id, instrument_id, open_time) DO UPDATE SET "
                "trade_count=EXCLUDED.trade_count, buy_trade_count=EXCLUDED.buy_trade_count, "
                "sell_trade_count=EXCLUDED.sell_trade_count, base_volume=EXCLUDED.base_volume, "
                "quote_volume=EXCLUDED.quote_volume, "
                "aggressive_buy_volume=EXCLUDED.aggressive_buy_volume, "
                "aggressive_sell_volume=EXCLUDED.aggressive_sell_volume, "
                "signed_volume=EXCLUDED.signed_volume, vwap=EXCLUDED.vwap, "
                "min_price=EXCLUDED.min_price, max_price=EXCLUDED.max_price, "
                "first_price=EXCLUDED.first_price, last_price=EXCLUDED.last_price, "
                "largest_trade=EXCLUDED.largest_trade",
                in.exchange_id, in.instrument_id, from, to);
        });
    }

    // Book aggregates: point-in-time fields = last sample in the window; sampled
    // snapshots already carry best/mid/microprice/spread/depth/imbalance.
    void roll_books(const Inst& in, int64_t to, const char* table, const char* trunc,
                    bool with_class) {
        persistence::with_txn_logged(pool_, "agg_books", [&](pqxx::work& txn) {
            const char* class_pred = with_class
                                         ? (in.retention_class == 1 ? " AND retention_class = 1"
                                                                    : " AND retention_class = 0")
                                         : "";
            const int64_t from = start_from(txn, table, in, class_pred, to);
            const std::string class_col = with_class ? "$3, " : "";
            const std::string conflict_key =
                with_class ? "(retention_class, exchange_id, instrument_id, open_time)"
                           : "(exchange_id, instrument_id, open_time)";
            std::string sql =
                "INSERT INTO " + std::string(table) + " (" + (with_class ? "retention_class, " : "") +
                "exchange_id, instrument_id, open_time, best_bid, best_ask, mid_price, spread, "
                "relative_spread, bid_depth_15, ask_depth_15, book_imbalance_15, microprice, "
                "snapshot_count, quality_status) "
                "SELECT " + class_col + "$1, $2, w, best_bid, best_ask, mid, best_ask-best_bid, "
                "CASE WHEN mid>0 THEN (best_ask-best_bid)/mid END, bid_depth_n, ask_depth_n, "
                "imbalance_n, microprice, cnt, 'ok' FROM ("
                "  SELECT DISTINCT ON (date_trunc('" + std::string(trunc) + "', snapshot_time)) "
                "    date_trunc('" + std::string(trunc) + "', snapshot_time) AS w, "
                "    best_bid, best_ask, mid, microprice, bid_depth_n, ask_depth_n, imbalance_n, "
                "    count(*) OVER (PARTITION BY date_trunc('" + std::string(trunc) +
                "', snapshot_time)) AS cnt "
                "  FROM market_orderbook_snapshots "
                "  WHERE exchange_id=$1 AND instrument_id=$2 "
                "    AND snapshot_time >= to_timestamp($" + std::string(with_class ? "4" : "3") +
                "::bigint/1000.0) "
                "    AND snapshot_time <  to_timestamp($" + std::string(with_class ? "5" : "4") +
                "::bigint/1000.0)" + class_pred +
                "  ORDER BY date_trunc('" + std::string(trunc) +
                "', snapshot_time), snapshot_time DESC"
                ") s ON CONFLICT " + conflict_key + " DO UPDATE SET "
                "best_bid=EXCLUDED.best_bid, best_ask=EXCLUDED.best_ask, mid_price=EXCLUDED.mid_price, "
                "spread=EXCLUDED.spread, relative_spread=EXCLUDED.relative_spread, "
                "bid_depth_15=EXCLUDED.bid_depth_15, ask_depth_15=EXCLUDED.ask_depth_15, "
                "book_imbalance_15=EXCLUDED.book_imbalance_15, microprice=EXCLUDED.microprice, "
                "snapshot_count=EXCLUDED.snapshot_count";
            if (with_class) {
                txn.exec_params(sql, in.exchange_id, in.instrument_id, in.retention_class, from, to);
            } else {
                txn.exec_params(sql, in.exchange_id, in.instrument_id, from, to);
            }
        });
    }

    void roll_books_1s(const Inst& in, int64_t to) {
        roll_books(in, to, "market_book_aggregates_1s", "second", true);
    }
    void roll_books_1m(const Inst& in, int64_t to) {
        roll_books(in, to, "market_book_aggregates_1m", "minute", false);
    }

    persistence::PgPool& pool_;
    int64_t lag_sec_ = 5;
    int64_t overlap_sec_ = 120;
    int64_t max_window_min_ = 240;
};

} // namespace

namespace altspotter::components {

int microstructure_aggregation(config::ServiceContext ctx) {
    const int64_t cycle_sec = config::opt_int(ctx.service, "cycle_interval_sec", 30);
    persistence::PgPool pool(
        ctx.common.postgres_dsn,
        static_cast<size_t>(config::opt_int(ctx.service, "postgres_pool_size", 2)));
    if (auto probe = pool.with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); }); !probe) {
        log::critical("postgres_unavailable", {{"message", probe.error().message}});
        return 1;
    }

    Aggregator agg(pool, ctx);
    auto& shutdown = ShutdownToken::instance();
    while (!shutdown.stop_requested()) {
        auto instruments = load_active(pool);
        if (!instruments) {
            log::error("microagg_active_load_failed", {{"message", instruments.error().message}});
        } else if (!instruments->empty()) {
            const int64_t started = timeutil::now_ms();
            agg.run_cycle(*instruments);
            log::info("microagg_cycle_done", {{"instruments", instruments->size()},
                                              {"duration_ms", timeutil::now_ms() - started}});
        }
        if (shutdown.wait_for(std::chrono::seconds(cycle_sec))) break;
    }
    log::info("service_stopped", {});
    return 0;
}

}  // namespace altspotter::components
