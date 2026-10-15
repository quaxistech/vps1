#include "altstrategy/engine_io/market_reader.hpp"

#include "altstrategy/logging/logger.hpp"
#include "altstrategy/persistence/pg.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <system_error>

namespace altstrategy::engine_io {

namespace {
int64_t ms_of(const pqxx::field& f) {
    // epoch millis via SQL extract(epoch ...) done in the query; here it's numeric.
    return static_cast<int64_t>(std::llround(f.as<double>() * 1000.0));
}
}

std::vector<PathBar> MarketReader::read_candles(int64_t instrument_id, int64_t from_ms, int64_t to_ms) {
    std::vector<PathBar> bars;
    pool_.with_txn([&](pqxx::work& txn) {
        const std::string q =
            "SELECT extract(epoch from open_time) AS ot, open::float8, high::float8, low::float8, "
            "close::float8 FROM " + cfg_.candles_table +
            " WHERE exchange_id=$1 AND instrument_id=$2 AND open_time >= to_timestamp($3) "
            "AND open_time <= to_timestamp($4) ORDER BY open_time";
        for (const auto& row : txn.exec_params(q, cfg_.bitget_exchange_id, instrument_id,
                                               from_ms / 1000.0, to_ms / 1000.0)) {
            PathBar b;
            b.open_ms = ms_of(row[0]);
            b.open = row[1].as<double>(); b.high = row[2].as<double>();
            b.low = row[3].as<double>(); b.close = row[4].as<double>();
            bars.push_back(b);
        }
    });
    return bars;
}

std::vector<PathBar> MarketReader::read_trade_bars(int64_t instrument_id, int64_t from_ms, int64_t to_ms,
                                                   int64_t bucket_ms) {
    std::vector<PathBar> bars;
    pool_.with_txn([&](pqxx::work& txn) {
        // Aggregate raw taker trades into fixed buckets → OHLC path (finer than 1m).
        const std::string q =
            "SELECT floor(extract(epoch from trade_time)*1000/$5)*$5 AS bucket, "
            "(array_agg(price::float8 ORDER BY trade_time))[1] AS o, max(price::float8) AS h, "
            "min(price::float8) AS l, (array_agg(price::float8 ORDER BY trade_time DESC))[1] AS c "
            "FROM " + cfg_.trades_table +
            " WHERE exchange_id=$1 AND instrument_id=$2 AND trade_time >= to_timestamp($3) "
            "AND trade_time <= to_timestamp($4) GROUP BY bucket ORDER BY bucket";
        for (const auto& row : txn.exec_params(q, cfg_.bitget_exchange_id, instrument_id,
                                               from_ms / 1000.0, to_ms / 1000.0, bucket_ms)) {
            PathBar b;
            b.open_ms = static_cast<int64_t>(row[0].as<double>());
            b.open = row[1].as<double>(); b.high = row[2].as<double>();
            b.low = row[3].as<double>(); b.close = row[4].as<double>();
            bars.push_back(b);
        }
    });
    return bars;
}

std::vector<MarketReader::VolumeMinute> MarketReader::read_volume_minutes(int64_t instrument_id,
                                                                         int64_t from_ms,
                                                                         int64_t to_ms) {
    std::vector<VolumeMinute> out;
    pool_.with_txn([&](pqxx::work& txn) {
        const std::string q =
            "SELECT extract(epoch from open_time) AS ot, quote_volume::float8, base_volume::float8 FROM " +
            cfg_.candles_table +
            " WHERE exchange_id=$1 AND instrument_id=$2 AND is_closed AND quality_status = 'ok' "
            "AND open_time >= to_timestamp($3) AND open_time < to_timestamp($4) ORDER BY open_time";
        for (const auto& row : txn.exec_params(q, cfg_.bitget_exchange_id, instrument_id,
                                               from_ms / 1000.0, to_ms / 1000.0)) {
            VolumeMinute m;
            m.open_ms = ms_of(row[0]);
            m.quote_volume = row[1].as<double>();
            m.base_volume = row[2].as<double>();
            out.push_back(m);
        }
    });
    return out;
}

std::optional<PriceTickTable> MarketReader::read_price_ticks() {
    PriceTickTable table;
    const auto r = pool_.with_txn([&](pqxx::work& txn) {
        const std::string q =
            "SELECT m.canonical_pair_id, v.version, "
            "(extract(epoch FROM v.valid_from) * 1000)::bigint, v.spec->>'tick_size' "
            "FROM " + cfg_.pair_map_table + " m JOIN " + cfg_.instrument_specs_table +
            " v ON v.exchange_instrument_id = m.exchange_instrument_id WHERE m.exchange_id = $1";
        for (const auto& row : txn.exec_params(q, cfg_.bitget_exchange_id)) {
            if (row[3].is_null()) continue;
            // The registry stores the tick as an exact decimal string; a value
            // that does not parse as a positive number is no grid at all.
            const std::string s = row[3].as<std::string>();
            double tick = 0;
            const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), tick);
            if (ec != std::errc{} || end != s.data() + s.size() || !std::isfinite(tick) || !(tick > 0)) continue;
            table[row[0].as<int64_t>()].push_back(
                PriceTickVersion{row[2].as<int64_t>(), row[1].as<int64_t>(), tick});
        }
    });
    if (!r) {
        log::error("price_tick_read_failed", {{"message", r.error().message}});
        return std::nullopt;
    }
    return table;
}

PricePath MarketReader::read_price_path(int64_t instrument_id, int64_t from_ms, int64_t to_ms) {
    constexpr int64_t kMin = 60'000;
    PricePath p;
    struct Minute { PathBar bar; bool traded = false; };
    std::map<int64_t, Minute> minutes;
    const auto r = pool_.with_txn([&](pqxx::work& txn) {
        const std::string q =
            "SELECT extract(epoch from open_time) AS ot, open::float8, high::float8, low::float8, "
            "close::float8, COALESCE(base_volume, 0)::float8 FROM " + cfg_.candles_table +
            " WHERE exchange_id=$1 AND instrument_id=$2 AND open_time >= to_timestamp($3::float8) "
            "AND open_time < to_timestamp($4::float8) ORDER BY open_time";
        for (const auto& row : txn.exec_params(q, cfg_.bitget_exchange_id, instrument_id,
                                               static_cast<double>(from_ms / kMin * kMin) / 1000.0,
                                               static_cast<double>(to_ms) / 1000.0)) {
            Minute m;
            m.bar.open_ms = ms_of(row[0]);
            m.bar.open = row[1].as<double>(); m.bar.high = row[2].as<double>();
            m.bar.low = row[3].as<double>(); m.bar.close = row[4].as<double>();
            m.traded = row[5].as<double>() > 0;
            minutes[m.bar.open_ms] = m;
        }
    });
    if (!r) {
        log::warn("price_path_read_failed", {{"pair", instrument_id}, {"message", r.error().message}});
        return p;   // empty: the evaluator reports no_path, the maturation retries
    }
    std::vector<std::pair<int64_t, int64_t>> candle_gaps;
    for (int64_t t = from_ms / kMin * kMin; t < to_ms; t += kMin)
        if (minutes.find(t) == minutes.end()) candle_gaps.emplace_back(t, t + kMin);

    if (cfg_.prefer_raw_trades && candle_gaps.empty()) {
        auto fine = read_trade_bars(instrument_id, from_ms, to_ms, 1000);
        std::set<int64_t> traded_minutes;
        for (const auto& b : fine) traded_minutes.insert(b.open_ms / kMin * kMin);
        // Full minutes only: the trades of a minute cut by either end of the
        // window may all lie outside it.
        bool complete = !fine.empty();
        for (const auto& [t, m] : minutes)
            if (m.traded && t >= from_ms && t + kMin <= to_ms && traded_minutes.count(t) == 0) {
                complete = false;
                break;
            }
        if (complete) {
            p.bars = std::move(fine);
            p.source = OutcomePath::raw_trades;
            p.bar_ms = 1000;
            // Every minute of [from, to) has its candle and every traded one
            // its trades: a second with no bucket is a second with no trade.
            p.covered_until_ms = to_ms;
            return p;
        }
    }
    for (auto& [t, m] : minutes) p.bars.push_back(m.bar);
    p.source = OutcomePath::m1_candles;
    p.bar_ms = kMin;
    p.gaps = std::move(candle_gaps);
    p.covered_until_ms = to_ms;   // every missing minute is in `gaps`
    return p;
}

PathResult MarketReader::read_path(int64_t instrument_id, int64_t from_ms, int64_t to_ms) {
    PathResult r;
    if (cfg_.prefer_raw_trades && raw_trades_cover(instrument_id, from_ms)) {
        // 1s trade-aggregate path (raw-trade precision) when trades are present.
        auto fine = read_trade_bars(instrument_id, from_ms, to_ms, 1000);
        if (fine.size() >= 2) { r.bars = std::move(fine); r.source = OutcomePath::raw_trades; return r; }
    }
    r.bars = read_candles(instrument_id, from_ms, to_ms);
    r.source = OutcomePath::m1_candles;
    return r;
}

// Whether raw trades still reach back to `from_ms` for this instrument.
//
// A path source must cover the START of the window, not merely return rows
// somewhere inside it. Raw trades are retained for hours while an outcome
// resolves after its holding time — measured here, trades reached back 6 h
// against an 8 h horizon — so a query over [entry, time_stop] returned plenty of
// recent buckets and NONE around the entry. The entry test then found no bar in
// its window and reported ENTRY_NOT_REACHED for everything, including market
// orders, which fill by definition: 250 of 250 market proposals "never filled",
// and the handful that did resolve were only those whose entry happened to fall
// inside the surviving trade window — a biased sample that made the strategy
// look anti-predictive when the measurement was simply blind.
bool MarketReader::raw_trades_cover(int64_t instrument_id, int64_t from_ms) {
    bool covered = false;
    pool_.with_txn([&](pqxx::work& txn) {
        const std::string q =
            "SELECT EXISTS (SELECT 1 FROM " + cfg_.trades_table +
            " WHERE exchange_id=$1 AND instrument_id=$2 AND trade_time <= to_timestamp($3))";
        covered = txn.exec_params(q, cfg_.bitget_exchange_id, instrument_id, from_ms / 1000.0)[0][0]
                      .as<bool>();
    });
    return covered;
}

} // namespace altstrategy::engine_io
