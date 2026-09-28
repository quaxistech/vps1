#include "persistence/candle_store.hpp"

#include "persistence/pg.hpp"
#include "core/time.hpp"

#include <sstream>

namespace altspotter::persistence {

namespace {

void append_common_values(std::ostringstream& sql, pqxx::transaction_base& txn, const CandleRow& r) {
    const Candle& c = r.candle;
    sql << r.exchange_id << ',' << r.instrument_id << ',' << ts_literal(txn, c.open_time_ms) << ','
        << ts_literal(txn, c.close_time_ms) << ',' << txn.quote(c.open.to_string()) << "::numeric,"
        << txn.quote(c.high.to_string()) << "::numeric," << txn.quote(c.low.to_string())
        << "::numeric," << txn.quote(c.close.to_string()) << "::numeric,"
        << txn.quote(c.base_volume.to_string()) << "::numeric,"
        << txn.quote(c.quote_volume.to_string()) << "::numeric,";
    if (c.trade_count) {
        sql << *c.trade_count << ',';
    } else {
        sql << "NULL,";
    }
    if (c.taker_buy_base_volume) {
        sql << txn.quote(c.taker_buy_base_volume->to_string()) << "::numeric,";
    } else {
        sql << "NULL,";
    }
    if (c.taker_sell_base_volume) {
        sql << txn.quote(c.taker_sell_base_volume->to_string()) << "::numeric,";
    } else {
        sql << "NULL,";
    }
    sql << (c.is_closed ? "true" : "false") << ','
        << txn.quote(std::string(source_label(c.source))) << ','
        << txn.quote(std::string(quality_label(c.quality))) << ',';
    if (r.exchange_received_at_ms > 0) {
        sql << ts_literal(txn, r.exchange_received_at_ms);
    } else {
        sql << "NULL";
    }
}

Result<WriteStats> run_upsert(pqxx::transaction_base& txn, const std::string& sql, size_t total) {
    try {
        const pqxx::result res = txn.exec(sql);
        WriteStats stats;
        for (const auto& row : res) {
            if (row[0].as<bool>()) {
                ++stats.inserted;
            } else {
                ++stats.updated;
            }
        }
        stats.identical = static_cast<int64_t>(total) - stats.inserted - stats.updated;
        return stats;
    } catch (const pqxx::broken_connection&) {
        throw; // let PgPool::with_txn classify and discard
    } catch (const pqxx::transaction_rollback&) {
        throw; // deadlock/serialization: with_txn classifies as retryable
    } catch (const pqxx::sql_error& e) {
        return fail(ErrorCode::database_error,
                    std::string("candle upsert failed: ") + e.what());
    }
}

} // namespace

void dedupe_candle_rows(std::vector<CandleRow>& rows, bool with_timeframe) {
    std::map<std::tuple<int16_t, int64_t, int32_t, int64_t>, size_t> last;
    for (size_t i = 0; i < rows.size(); ++i) {
        last[{rows[i].exchange_id, rows[i].instrument_id,
              with_timeframe ? tf_minutes(rows[i].candle.timeframe) : 0,
              rows[i].candle.open_time_ms}] = i;
    }
    if (last.size() == rows.size()) return;
    std::vector<CandleRow> unique;
    unique.reserve(last.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        auto it = last.find({rows[i].exchange_id, rows[i].instrument_id,
                             with_timeframe ? tf_minutes(rows[i].candle.timeframe) : 0,
                             rows[i].candle.open_time_ms});
        if (it != last.end() && it->second == i) unique.push_back(std::move(rows[i]));
    }
    rows = std::move(unique);
}

Result<WriteStats> upsert_candles_1m(pqxx::transaction_base& txn,
                                     const std::vector<CandleRow>& rows) {
    if (rows.empty()) return WriteStats{};

    std::ostringstream sql;
    sql << "INSERT INTO market_candles_1m (exchange_id, instrument_id, open_time, close_time, "
           "open, high, low, close, base_volume, quote_volume, trade_count, "
           "taker_buy_base_volume, taker_sell_base_volume, is_closed, source, quality_status, "
           "exchange_received_at) VALUES ";
    bool first = true;
    for (const auto& r : rows) {
        sql << (first ? "(" : ",(");
        first = false;
        append_common_values(sql, txn, r);
        sql << ')';
    }
    sql << " ON CONFLICT (exchange_id, instrument_id, open_time) DO UPDATE SET "
           "close_time = EXCLUDED.close_time, open = EXCLUDED.open, high = EXCLUDED.high, "
           "low = EXCLUDED.low, close = EXCLUDED.close, base_volume = EXCLUDED.base_volume, "
           "quote_volume = EXCLUDED.quote_volume, "
           "trade_count = COALESCE(EXCLUDED.trade_count, market_candles_1m.trade_count), "
           "taker_buy_base_volume = COALESCE(EXCLUDED.taker_buy_base_volume, "
           "market_candles_1m.taker_buy_base_volume), "
           "taker_sell_base_volume = COALESCE(EXCLUDED.taker_sell_base_volume, "
           "market_candles_1m.taker_sell_base_volume), "
           "is_closed = EXCLUDED.is_closed, source = EXCLUDED.source, "
           "quality_status = EXCLUDED.quality_status, "
           "revision = market_candles_1m.revision + 1, updated_at = now() "
           "WHERE (market_candles_1m.open, market_candles_1m.high, market_candles_1m.low, "
           "market_candles_1m.close, market_candles_1m.base_volume, "
           "market_candles_1m.quote_volume, market_candles_1m.is_closed) IS DISTINCT FROM "
           "(EXCLUDED.open, EXCLUDED.high, EXCLUDED.low, EXCLUDED.close, EXCLUDED.base_volume, "
           "EXCLUDED.quote_volume, EXCLUDED.is_closed) "
           "RETURNING (revision = 0) AS inserted";
    return run_upsert(txn, sql.str(), rows.size());
}

Result<WriteStats> upsert_candles_agg(pqxx::transaction_base& txn,
                                      const std::vector<CandleRow>& rows) {
    if (rows.empty()) return WriteStats{};

    std::ostringstream sql;
    sql << "INSERT INTO market_candles_agg (exchange_id, instrument_id, timeframe, open_time, "
           "close_time, open, high, low, close, base_volume, quote_volume, trade_count, "
           "taker_buy_base_volume, taker_sell_base_volume, is_closed, source, quality_status, "
           "exchange_received_at, minute_count) VALUES ";
    bool first = true;
    for (const auto& r : rows) {
        sql << (first ? "(" : ",(");
        first = false;
        sql << r.exchange_id << ',' << r.instrument_id << ','
            << txn.quote(std::string(tf_label(r.candle.timeframe))) << ',';
        // Reuse common column ordering by emitting remaining fields inline.
        const Candle& c = r.candle;
        sql << ts_literal(txn, c.open_time_ms) << ',' << ts_literal(txn, c.close_time_ms) << ','
            << txn.quote(c.open.to_string()) << "::numeric," << txn.quote(c.high.to_string())
            << "::numeric," << txn.quote(c.low.to_string()) << "::numeric,"
            << txn.quote(c.close.to_string()) << "::numeric,"
            << txn.quote(c.base_volume.to_string()) << "::numeric,"
            << txn.quote(c.quote_volume.to_string()) << "::numeric,";
        if (c.trade_count) {
            sql << *c.trade_count << ',';
        } else {
            sql << "NULL,";
        }
        if (c.taker_buy_base_volume) {
            sql << txn.quote(c.taker_buy_base_volume->to_string()) << "::numeric,";
        } else {
            sql << "NULL,";
        }
        if (c.taker_sell_base_volume) {
            sql << txn.quote(c.taker_sell_base_volume->to_string()) << "::numeric,";
        } else {
            sql << "NULL,";
        }
        sql << (c.is_closed ? "true" : "false") << ','
            << txn.quote(std::string(source_label(c.source))) << ','
            << txn.quote(std::string(quality_label(c.quality))) << ",NULL,"
            << (c.minute_count ? std::to_string(*c.minute_count) : std::string("NULL"));
        sql << ')';
    }
    sql << " ON CONFLICT (exchange_id, instrument_id, timeframe, open_time) DO UPDATE SET "
           "close_time = EXCLUDED.close_time, open = EXCLUDED.open, high = EXCLUDED.high, "
           "low = EXCLUDED.low, close = EXCLUDED.close, base_volume = EXCLUDED.base_volume, "
           "quote_volume = EXCLUDED.quote_volume, trade_count = EXCLUDED.trade_count, "
           "taker_buy_base_volume = EXCLUDED.taker_buy_base_volume, "
           "taker_sell_base_volume = EXCLUDED.taker_sell_base_volume, "
           "is_closed = EXCLUDED.is_closed, source = EXCLUDED.source, "
           "quality_status = EXCLUDED.quality_status, minute_count = EXCLUDED.minute_count, "
           "revision = market_candles_agg.revision + 1, updated_at = now() "
           "WHERE (market_candles_agg.open, market_candles_agg.high, market_candles_agg.low, "
           "market_candles_agg.close, market_candles_agg.base_volume, "
           "market_candles_agg.quote_volume, market_candles_agg.quality_status) "
           "IS DISTINCT FROM (EXCLUDED.open, EXCLUDED.high, EXCLUDED.low, EXCLUDED.close, "
           "EXCLUDED.base_volume, EXCLUDED.quote_volume, EXCLUDED.quality_status) "
           "RETURNING (revision = 0) AS inserted";
    return run_upsert(txn, sql.str(), rows.size());
}

} // namespace altspotter::persistence
