#include "venue_store.hpp"

#include "reference_features.hpp"

#include "alttech/persistence/sql_literal.hpp"
#include "alttech/persistence/analytics_outbox.hpp"
#include "alttech/messaging/codec.hpp"
#include "alttech/messaging/topology.hpp"
#include "alttech/time/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace alttech::venuelite::store {
namespace {
using persistence::sql::append_float_array;
using persistence::sql::append_bytea;


// Compact reference-venue microstructure layout, derived entirely from
// AltSpotter's 1m trade and book aggregates.
constexpr std::size_t kMicroCount = 11;
constexpr const char* kMicroNames[kMicroCount] = {
    "trade_imbalance", "signed_volume", "cvd", "trade_intensity", "spread", "relative_spread",
    "bid_depth5_usdt", "ask_depth5_usdt", "book_imbalance", "microprice_rel", "data_quality"};

std::optional<double> opt_num(const pqxx::field& f) {
    if (f.is_null()) return std::nullopt;
    return f.as<double>();
}



Result<int64_t> ensure_named_set(persistence::PgPool& pool, const std::string& name,
                                 const std::vector<std::string>& members, const std::string& hash) {
    int64_t set_id = 0;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT id,schema_hash FROM alttech.feature_set_versions WHERE name = $1 AND version = $2", name,
            kReferenceSetVersion);
        if (!rows.empty()) {
            if (rows[0][1].as<std::string>() != hash) throw std::runtime_error("frozen reference feature schema mismatch: " + name);
            set_id = rows[0][0].as<int64_t>();
            return;
        }
        const auto ins = txn.exec_params(
            "INSERT INTO alttech.feature_set_versions "
            "(name, version, scope, profile, engine_version, schema_hash, frozen) "
            "VALUES ($1, $2, 'VENUE', 'reference_lite', $3, $4, true) RETURNING id",
            name, kReferenceSetVersion, std::string(kReferenceEngine), hash);
        set_id = ins[0][0].as<int64_t>();
        for (size_t i = 0; i < members.size(); ++i) {
            const auto def = txn.exec_params(
                "INSERT INTO alttech.feature_definitions (name, family, scope) "
                "VALUES ($1, 'reference_lite', 'VENUE') "
                "ON CONFLICT (name) DO UPDATE SET family = EXCLUDED.family RETURNING id",
                members[i]);
            const auto ps = txn.exec_params(
                "INSERT INTO alttech.feature_parameter_sets (definition_id, parameters) "
                "VALUES ($1, '{}') ON CONFLICT (definition_id, parameters) "
                "DO UPDATE SET parameters = EXCLUDED.parameters RETURNING id",
                def[0][0].as<int64_t>());
            txn.exec_params(
                "INSERT INTO alttech.feature_set_members "
                "(feature_set_version_id, position, parameter_set_id, output_name) "
                "VALUES ($1, $2, $3, $4)",
                set_id, static_cast<int>(i), ps[0][0].as<int64_t>(), members[i]);
        }
    });
    if (!res) return std::unexpected(res.error());
    return set_id;
}

} // namespace

Result<std::vector<PairVenue>> load_pair_venues(persistence::PgPool& pool, VenueScope scope) {
    std::vector<PairVenue> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec(
            "SELECT u.canonical_pair_id, m.exchange_id, ei.exchange_symbol "
            "FROM global_observation_universe u "
            "JOIN canonical_pair_exchange_map m ON m.canonical_pair_id = u.canonical_pair_id "
            "JOIN exchange_instruments ei ON ei.id = m.exchange_instrument_id "
            "WHERE u.eligible AND ei.is_whitelisted AND ei.status = 'active' "
            "ORDER BY u.canonical_pair_id, m.exchange_id");
        for (const auto& r : rows) {
            const auto venue = venue_from_id(r[1].as<int>());
            if (!venue) continue;
            // The primary venue already has a full-engine product for the
            // outputs it owns; the sweep only fills what it does not cover.
            if (scope == VenueScope::references_only && is_primary(*venue)) continue;
            out.push_back({r[0].as<int64_t>(), *venue, r[2].as<std::string>()});
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<int64_t> ensure_reference_feature_set(persistence::PgPool& pool) {
    std::vector<std::string> members;
    std::string hash(kReferenceSetName);
    hash += ':';
    for (const auto& n : kReferenceFeatureNames) {
        members.emplace_back(n);
        hash.append(n).push_back('|');
    }
    return ensure_named_set(pool, std::string(kReferenceSetName), members, hash);
}

Result<int64_t> ensure_reference_micro_set(persistence::PgPool& pool) {
    std::vector<std::string> members;
    std::string hash(kReferenceMicroSetName);
    hash += ':';
    for (const auto* n : kMicroNames) {
        members.emplace_back(n);
        hash.append(n).push_back('|');
    }
    return ensure_named_set(pool, std::string(kReferenceMicroSetName), members, hash);
}

Result<std::optional<int64_t>> next_venue_bar(persistence::PgPool& pool, const PairVenue& pv,
                                              VenueOutput kind, int64_t watermark_ms,
                                              int lookback_min, int64_t feature_set_id) {
    std::optional<int64_t> out;
    const int64_t now = timeutil::now_ms();
    const int64_t horizon = now - static_cast<int64_t>(lookback_min) * 60'000;
    // The "already produced" gate, per output kind, on the bar's open_time.
    std::string done;
    switch (kind) {
        case VenueOutput::feature:
            // Scoped to THIS feature set. Without the scope the primary venue
            // is permanently "already done": its bars carry talib-core vectors
            // written by the feature module, so the sweep skipped every one of
            // them and the venue every order is sent to never received the
            // BBO/flow block that only this sweep produces.
            done = "SELECT 1 FROM alttech.venue_feature_vectors d WHERE d.exchange_id=$1 "
                   "AND d.canonical_pair_id=$2 AND d.timeframe='1m' "
                   "AND d.bar_open_time=c.open_time "
                   "AND d.feature_set_version_id=$5";
            break;
        case VenueOutput::regime:
            done = "SELECT 1 FROM alttech.venue_regimes d WHERE d.exchange_id=$1 "
                   "AND d.canonical_pair_id=$2 AND d.timeframe='1m' AND d.bar_open_time=c.open_time";
            break;
        case VenueOutput::microstructure:
            done = "SELECT 1 FROM alttech.venue_microstructure_vectors d "
                   "WHERE d.part_class='ref_1m' AND d.exchange_id=$1 "
                   "AND d.canonical_pair_id=$2 AND d.window_label='1m' "
                   "AND d.window_start=c.open_time";
            break;
        case VenueOutput::structure:
            // Structure zones evolve rather than being per-bar keyed; the sweep
            // processes the newest closed bar each cycle.
            done = "SELECT 0 WHERE false";
            break;
    }
    auto res = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        const std::string sql =
            "SELECT (extract(epoch FROM c.open_time) * 1000)::bigint FROM market_candles_1m c "
            "WHERE c.exchange_id = $1 AND c.instrument_id = $2 AND c.is_closed "
            "  AND c.open_time <= to_timestamp($3::bigint / 1000.0) "
            "  AND c.open_time >= to_timestamp($4::bigint / 1000.0) "
            "  AND NOT EXISTS (" + done + ") ORDER BY c.open_time DESC LIMIT 1";
        const auto r = kind == VenueOutput::feature
                           ? txn.exec_params(sql, venue_id(pv.venue), pv.canonical_pair_id,
                                             now - watermark_ms, horizon, feature_set_id)
                           : txn.exec_params(sql, venue_id(pv.venue), pv.canonical_pair_id,
                                             now - watermark_ms, horizon);
        if (!r.empty() && !r[0][0].is_null()) out = r[0][0].as<int64_t>();
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<std::vector<LiteCandle>> load_candle_window(persistence::PgPool& pool, const PairVenue& pv,
                                                   int64_t end_bar_ms, int count) {
    count = std::clamp(count, 2, 512);
    const int64_t start_ms = end_bar_ms - static_cast<int64_t>(count) * 60'000;
    std::vector<LiteCandle> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        for (const auto& r : txn.exec_params(
                 "SELECT (extract(epoch FROM open_time)*1000)::bigint, open::float8, "
                 "  high::float8, low::float8, close::float8, base_volume::float8, "
                 "  quote_volume::float8, COALESCE(taker_buy_base_volume, 0)::float8, "
                 "  COALESCE(taker_sell_base_volume, 0)::float8, revision, quality_status "
                 "FROM market_candles_1m WHERE exchange_id = $1 AND instrument_id = $2 "
                 "AND is_closed AND open_time > to_timestamp($3::bigint / 1000.0) "
                 "AND open_time <= to_timestamp($4::bigint / 1000.0) ORDER BY open_time",
                 venue_id(pv.venue), pv.canonical_pair_id, start_ms, end_bar_ms)) {
            LiteCandle c;
            c.open_time_ms = r[0].as<int64_t>();
            c.open = r[1].as<double>();
            c.high = r[2].as<double>();
            c.low = r[3].as<double>();
            c.close = r[4].as<double>();
            c.base_volume = r[5].as<double>();
            c.quote_volume = r[6].as<double>();
            c.taker_buy_base_volume = r[7].as<double>();
            c.taker_sell_base_volume = r[8].as<double>();
            c.revision = r[9].as<int64_t>();
            c.quality_ok = r[10].as<std::string>() == "ok";
            out.push_back(c);
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<LiteContext> load_context(persistence::PgPool& pool, const PairVenue& pv, int64_t bar_ms) {
    LiteContext ctx;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        const auto ta = txn.exec_params(
            "SELECT aggressive_buy_volume, aggressive_sell_volume, signed_volume, vwap, trade_count "
            "FROM market_trade_aggregates_1m WHERE exchange_id=$1 AND instrument_id=$2 "
            "AND open_time = to_timestamp($3::bigint/1000.0)",
            venue_id(pv.venue), pv.canonical_pair_id, bar_ms);
        if (!ta.empty()) {
            ctx.buy_volume = opt_num(ta[0][0]);
            ctx.sell_volume = opt_num(ta[0][1]);
            ctx.signed_volume_base = opt_num(ta[0][2]);
            ctx.vwap = opt_num(ta[0][3]);
            ctx.trade_count = opt_num(ta[0][4]);
        }
        const auto ba = txn.exec_params(
            "SELECT spread, relative_spread, bid_depth_5, ask_depth_5, book_imbalance_5, "
            "microprice, mid_price FROM market_book_aggregates_1m WHERE exchange_id=$1 "
            "AND instrument_id=$2 AND open_time = to_timestamp($3::bigint/1000.0)",
            venue_id(pv.venue), pv.canonical_pair_id, bar_ms);
        if (!ba.empty()) {
            ctx.spread = opt_num(ba[0][0]);
            ctx.relative_spread = opt_num(ba[0][1]);
            ctx.bid_depth5 = opt_num(ba[0][2]);
            ctx.ask_depth5 = opt_num(ba[0][3]);
            ctx.book_imbalance = opt_num(ba[0][4]);
            ctx.microprice = opt_num(ba[0][5]);
            ctx.mid_price = opt_num(ba[0][6]);
        }
        // Intra-minute top-of-book. Sampled continuously (206-909 updates per
        // minute), unlike the order-book snapshots above (~7 per minute), so it
        // is the only record of what the spread did INSIDE the bar.
        const auto bb = txn.exec_params(
            "SELECT bid_first, ask_first, bid_last, ask_last, spread_min, spread_max, "
            "       sample_count "
            "FROM market_bbo_1m WHERE exchange_id=$1 AND instrument_id=$2 "
            "AND open_time = to_timestamp($3::bigint/1000.0)",
            venue_id(pv.venue), pv.canonical_pair_id, bar_ms);
        if (!bb.empty()) {
            ctx.bbo_bid_first = opt_num(bb[0][0]);
            ctx.bbo_ask_first = opt_num(bb[0][1]);
            ctx.bbo_bid_last = opt_num(bb[0][2]);
            ctx.bbo_ask_last = opt_num(bb[0][3]);
            ctx.bbo_spread_min = opt_num(bb[0][4]);
            ctx.bbo_spread_max = opt_num(bb[0][5]);
            ctx.bbo_sample_count = opt_num(bb[0][6]);
        }
        // Depth comes from the SNAPSHOTS, not the aggregates.
        //
        // market_book_aggregates_1m and _1s carry spread and microprice but
        // leave every depth column NULL -- measured across all four venues,
        // 0 of 432 minute rows had bid_depth_5. Two features
        // (RF_DEPTH_USDT, RF_BOOK_IMBALANCE) were therefore invalid in every
        // vector ever published, while the underlying numbers sat in
        // market_orderbook_snapshots, where 3993 of 3993 rows carry them.
        // Reading them here is the fix that belongs in the consumer: the
        // snapshot is the authoritative record and AltSpotter is frozen.
        const auto sn = txn.exec_params(
            "SELECT avg(bid_depth_n), avg(ask_depth_n), avg(imbalance_n), "
            "       min(bid_depth_n + ask_depth_n), avg(bid_depth_n + ask_depth_n) "
            "FROM market_orderbook_snapshots "
            "WHERE exchange_id=$1 AND instrument_id=$2 "
            "  AND snapshot_time >= to_timestamp($3::bigint/1000.0) "
            "  AND snapshot_time <  to_timestamp($3::bigint/1000.0) + interval '1 minute'",
            venue_id(pv.venue), pv.canonical_pair_id, bar_ms);
        if (!sn.empty()) {
            if (!ctx.bid_depth5) ctx.bid_depth5 = opt_num(sn[0][0]);
            if (!ctx.ask_depth5) ctx.ask_depth5 = opt_num(sn[0][1]);
            if (!ctx.book_imbalance) ctx.book_imbalance = opt_num(sn[0][2]);
            ctx.book_depth_min_1s = opt_num(sn[0][3]);
            ctx.book_depth_mean_1s = opt_num(sn[0][4]);
        }

        // Second-level summary of this minute. One pass over the 1s series,
        // reduced to peak/mean/occupancy/dispersion -- the shape of the flow,
        // not a copy of it. This is the only stored source with sub-15s
        // resolution, and the only one that survives for replay.
        const auto ts = txn.exec_params(
            "SELECT max(trade_count), max(base_volume), avg(base_volume), "
            "       count(*) FILTER (WHERE trade_count > 0), stddev_pop(signed_volume) "
            "FROM market_trade_aggregates_1s "
            "WHERE exchange_id=$1 AND instrument_id=$2 "
            "  AND open_time >= to_timestamp($3::bigint/1000.0) "
            "  AND open_time <  to_timestamp($3::bigint/1000.0) + interval '1 minute'",
            venue_id(pv.venue), pv.canonical_pair_id, bar_ms);
        if (!ts.empty()) {
            ctx.flow_peak_trades_1s = opt_num(ts[0][0]);
            ctx.flow_peak_volume_1s = opt_num(ts[0][1]);
            ctx.flow_mean_volume_1s = opt_num(ts[0][2]);
            if (const auto active = opt_num(ts[0][3])) {
                ctx.flow_active_seconds = *active / 60.0;  // fraction of the minute
            }
            ctx.flow_signed_stddev = opt_num(ts[0][4]);
        }
        // Intra-minute depth extremes come from the SNAPSHOTS.
        //
        // market_book_aggregates_1s carries spread and microprice but leaves
        // every depth column NULL -- measured, 0 of 3560 rows across all four
        // venues -- so features built on it could never become valid. The
        // snapshots do carry depth (3993 of 3993 rows) at roughly 7 samples per
        // minute. Coarser than one-second resolution, and honestly so: this is
        // the finest depth series that actually exists.
        const auto dc = txn.exec_params(
            "SELECT mark_price, index_price, funding_rate, open_interest_quote, "
            "       open_interest_base "
            "FROM market_derivatives_context_1m WHERE exchange_id=$1 AND instrument_id=$2 "
            "AND open_time = to_timestamp($3::bigint/1000.0)",
            venue_id(pv.venue), pv.canonical_pair_id, bar_ms);
        std::optional<double> oi_now;
        std::optional<double> oi_now_base;
        if (!dc.empty()) {
            ctx.mark_price = opt_num(dc[0][0]);
            ctx.index_price = opt_num(dc[0][1]);
            ctx.funding_rate = opt_num(dc[0][2]);
            oi_now = opt_num(dc[0][3]);
            oi_now_base = opt_num(dc[0][4]);
        }
        if (ctx.funding_rate) {
            // The settlement interval differs per venue and per contract, so it
            // is measured from the venue's own settlement history rather than
            // assumed. Without history the funding feature stays invalid.
            const auto fi = txn.exec_params(
                "SELECT EXTRACT(EPOCH FROM (max(funding_time) - min(funding_time))) "
                "  / NULLIF(count(*) - 1, 0) / 3600.0 "
                "FROM (SELECT funding_time FROM market_funding_settlements "
                "      WHERE exchange_id = $1 AND instrument_id = $2 "
                "      ORDER BY funding_time DESC LIMIT 8) recent",
                venue_id(pv.venue), pv.canonical_pair_id);
            if (!fi.empty()) {
                const auto hours = opt_num(fi[0][0]);
                if (hours && *hours > 0.0) ctx.funding_interval_hours = hours;
            }
        }
        // Relative OI change, from whichever unit the venue actually reports.
        //
        // Only `open_interest_quote` used to be read, and it is empty on two of
        // four venues: measured over 30 minutes, quote was populated on 100% of
        // Bybit rows and 58% of OKX, and on 0% of Bitget and Binance, while
        // `open_interest_base` was populated on 96-100% everywhere. The feature
        // was therefore invalid in 833 of 833 vectors on the execution venue.
        //
        // A ratio is unit-invariant, but only if BOTH ends come from the same
        // column -- mixing base and quote would divide coins by dollars. The
        // pair is therefore chosen consistently, quote first, and left unset
        // when neither column has both ends.
        if ((oi_now && *oi_now > 0.0) || (oi_now_base && *oi_now_base > 0.0)) {
            const auto prev = txn.exec_params(
                "SELECT open_interest_quote, open_interest_base "
                "FROM market_derivatives_context_1m WHERE exchange_id=$1 "
                "AND instrument_id=$2 AND open_time = to_timestamp($3::bigint/1000.0)",
                venue_id(pv.venue), pv.canonical_pair_id, bar_ms - 60'000);
            if (!prev.empty()) {
                const auto prev_quote = opt_num(prev[0][0]);
                const auto prev_base = opt_num(prev[0][1]);
                if (oi_now && *oi_now > 0.0 && prev_quote && *prev_quote > 0.0) {
                    ctx.oi_change_ratio = (*oi_now - *prev_quote) / *prev_quote;
                } else if (oi_now_base && *oi_now_base > 0.0 && prev_base && *prev_base > 0.0) {
                    ctx.oi_change_ratio = (*oi_now_base - *prev_base) / *prev_base;
                }
            }
        }
        const auto q = txn.exec_params(
            "SELECT quality FROM exchange_data_quality WHERE exchange_id=$1 AND instrument_id=$2",
            venue_id(pv.venue), pv.canonical_pair_id);
        if (!q.empty()) {
            const std::string s = q[0][0].as<std::string>();
            ctx.data_quality = s == "VALID" ? 1.0 : (s == "DEGRADED" ? 0.5 : 0.0);
        }
    });
    if (!res) return std::unexpected(res.error());
    return ctx;
}

Result<void> write_venue_feature(persistence::PgPool& pool, const PairVenue& pv, int64_t bar_ms,
                                 int64_t revision, int64_t feature_set_id, const RefResult& res) {
    return pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        std::string sql =
            "INSERT INTO alttech.venue_feature_vectors (venue_class, exchange_id, "
            "canonical_pair_id, timeframe, bar_open_time, bar_revision, "
            "feature_set_version_id, profile, values, validity_bitmap, status, "
            "venue_quality, engine_version, computed_at) VALUES (";
        const std::string status = res.warming ? "WARMING" : "VALID";
        sql += txn.quote(std::string(venue_feature_class(pv.venue))) + "," +
               std::to_string(venue_id(pv.venue)) + "," + std::to_string(pv.canonical_pair_id) +
               ",'1m',to_timestamp(" + std::to_string(bar_ms) + "::bigint/1000.0)," +
               std::to_string(revision) + "," + std::to_string(feature_set_id) +
               ",'reference_lite',";
        append_float_array(sql, res.values);
        sql += ",";
        append_bytea(sql, res.validity);
        sql += "," + txn.quote(status) + ",'VALID'," + txn.quote(std::string(kReferenceEngine)) +
               ",now())";
        sql +=
            " ON CONFLICT (venue_class, exchange_id, canonical_pair_id, timeframe, bar_open_time, "
            "bar_revision, feature_set_version_id) DO UPDATE SET values = EXCLUDED.values, "
            "validity_bitmap = EXCLUDED.validity_bitmap, status = EXCLUDED.status, "
            "updated_at = now()";
        txn.exec(sql);
    });
}

Result<void> write_venue_regime(persistence::PgPool& pool, const PairVenue& pv, int64_t bar_ms,
                                const RefResult& res) {
    return pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        char axes[256];
        std::snprintf(axes, sizeof(axes),
                      "{\"trend_strength\":%.6g,\"volatility_regime\":%.6g,\"liquidity_quality\":%.6g}",
                      res.trend_strength, res.volatility_regime, res.liquidity_quality);
        txn.exec_params(
            "INSERT INTO alttech.venue_regimes (exchange_id, canonical_pair_id, "
            "timeframe, bar_open_time, regime, confidence, axes, venue_quality, "
            "algorithm_version, computed_at) "
            "VALUES ($1,$2,'1m',to_timestamp($3::bigint/1000.0),$4,$5,$6::jsonb,"
            "'VALID','reference-regime-1',now()) "
            "ON CONFLICT (exchange_id, canonical_pair_id, timeframe, bar_open_time) DO UPDATE SET "
            "regime = EXCLUDED.regime, confidence = EXCLUDED.confidence, axes = EXCLUDED.axes, "
            "computed_at = now()",
            venue_id(pv.venue), pv.canonical_pair_id, bar_ms,
            std::string(regime_label(res.regime)), res.regime_confidence, std::string(axes));
    });
}

Result<void> write_venue_micro(persistence::PgPool& pool, const PairVenue& pv, int64_t bar_ms,
                               int64_t feature_set_id, const LiteContext& ctx, double close,
                               std::vector<float>* out_values, std::vector<uint8_t>* out_validity) {
    std::vector<float> values(kMicroCount, 0.0F);
    std::vector<uint8_t> validity((kMicroCount + 7) / 8, 0);
    const auto set = [&](std::size_t i, std::optional<double> v) {
        if (!v || !std::isfinite(*v)) return;
        values[i] = static_cast<float>(*v);
        validity[i / 8] |= static_cast<uint8_t>(1U << (i % 8));
    };
    std::optional<double> imbalance;
    if (ctx.buy_volume && ctx.sell_volume && (*ctx.buy_volume + *ctx.sell_volume) > 0.0) {
        imbalance = (*ctx.buy_volume - *ctx.sell_volume) / (*ctx.buy_volume + *ctx.sell_volume);
    }
    std::optional<double> signed_usdt;
    if (ctx.signed_volume_base) signed_usdt = *ctx.signed_volume_base * close;
    std::optional<double> micro_rel;
    if (ctx.microprice && ctx.mid_price && *ctx.mid_price > 0.0) micro_rel = *ctx.microprice / *ctx.mid_price - 1.0;
    set(0, imbalance);
    set(1, signed_usdt);
    set(2, signed_usdt); // cvd (per-minute)
    set(3, ctx.trade_count);
    set(4, ctx.spread);
    set(5, ctx.relative_spread);
    set(6, ctx.bid_depth5 ? std::optional<double>(*ctx.bid_depth5 * close) : std::nullopt);
    set(7, ctx.ask_depth5 ? std::optional<double>(*ctx.ask_depth5 * close) : std::nullopt);
    set(8, ctx.book_imbalance);
    set(9, micro_rel);
    set(10, ctx.data_quality);

    auto written = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        std::string sql =
            "INSERT INTO alttech.venue_microstructure_vectors (part_class, exchange_id, "
            "canonical_pair_id, window_label, window_start, feature_set_version_id, "
            "profile, values, validity_bitmap, status, venue_quality, engine_version, computed_at) "
            "VALUES ('ref_1m',";
        sql += std::to_string(venue_id(pv.venue)) + "," +
               std::to_string(pv.canonical_pair_id) + ",'1m',to_timestamp(" +
               std::to_string(bar_ms) + "::bigint/1000.0)," +
               std::to_string(feature_set_id) + ",'reference_lite',";
        append_float_array(sql, values);
        sql += ",";
        append_bytea(sql, validity);
        sql += ",'VALID','VALID'," + txn.quote(std::string(kReferenceEngine)) + ",now())";
        sql +=
            " ON CONFLICT (part_class, exchange_id, canonical_pair_id, window_label, window_start, "
            "feature_set_version_id) DO UPDATE SET values = EXCLUDED.values, "
            "validity_bitmap = EXCLUDED.validity_bitmap, computed_at = now()";
        txn.exec(sql);
        const auto manifest = txn.exec_params("SELECT version,schema_hash,engine_version FROM alttech.feature_set_versions WHERE id=$1", feature_set_id);
        if (manifest.size() != 1) throw std::runtime_error("reference micro feature manifest is missing");
        messaging::EnvelopeMeta meta;
        meta.venue = pv.venue;
        meta.event_type = "microfeature.venue";
        meta.instrument_id = pv.canonical_pair_id;
        meta.exchange_symbol = pv.exchange_symbol;
        meta.exchange_timestamp_ms = bar_ms;
        meta.source_service = "alttech-service";
        meta.message_id = "vm:" + std::string(venue_code(pv.venue)) + ":" + pv.exchange_symbol +
            ":1m:" + std::to_string(bar_ms) + ":set" + std::to_string(feature_set_id);
        auto envelope = messaging::make_envelope(meta);
        envelope.set_canonical_pair_id(pv.canonical_pair_id);
        auto* payload = envelope.mutable_venue_microstructure_vector();
        payload->set_scope(::alttech::v1::ANALYTICS_SCOPE_VENUE);
        payload->set_exchange(envelope.exchange());
        payload->set_canonical_pair_id(pv.canonical_pair_id);
        payload->set_instrument_id(pv.canonical_pair_id);
        payload->set_window_label("1m");
        payload->set_window_start_ms(bar_ms);
        payload->set_profile("reference_lite");
        payload->set_feature_set_name(std::string(kReferenceMicroSetName));
        payload->set_feature_set_version(manifest[0][0].as<int>());
        payload->set_schema_hash(manifest[0][1].as<std::string>());
        payload->set_engine_version(manifest[0][2].as<std::string>());
        for (float value : values) payload->add_values(value);
        payload->set_validity_bitmap(std::string(validity.begin(), validity.end()));
        payload->set_status(::alttech::v1::ANALYTICS_VECTOR_STATUS_VALID);
        payload->set_venue_quality(ctx.data_quality >= 1.0 ?
            ::alttech::v1::QUALITY_STATE_VALID : ::alttech::v1::QUALITY_STATE_DEGRADED);
        envelope.set_quality_state(payload->venue_quality());
        payload->set_computed_at_ms(timeutil::now_ms());
        persistence::enqueue_analytics(txn, envelope.message_id(),
            messaging::topology::venue_microfeature_subject(pv.venue, pv.exchange_symbol, "1m"),
            messaging::serialize(envelope));
    });
    if (written) {
        if (out_values != nullptr) *out_values = values;
        if (out_validity != nullptr) *out_validity = validity;
    }
    return written;
}

Result<void> write_venue_zone(persistence::PgPool& pool, const PairVenue& pv,
                              const SwingZone& zone) {
    const std::string kind = zone.resistance ? "resistance" : "support";
    const double center = zone.center;
    return pool.with_txn([&](pqxx::work& txn) {
        // One evolving zone per (venue, pair, tf, kind): upsert by refreshing the
        // most recent matching ACTIVE/CANDIDATE zone or inserting a new one.
        const auto existing = txn.exec_params(
            // TESTED must be in this list. The lifecycle is now derived from
            // evidence, so a zone that earns two rejections becomes TESTED — and
            // if the lookup cannot see it, every subsequent cycle inserts a
            // fresh row instead of refreshing it and the table grows without
            // bound. Only terminal states (BROKEN, EXPIRED, FLIPPED) start a new
            // zone.
            "SELECT id FROM alttech.venue_structure_zones WHERE exchange_id=$1 "
            "AND canonical_pair_id=$2 AND timeframe='1m' AND kind=$3 "
            "AND lifecycle IN ('CANDIDATE','ACTIVE','TESTED') "
            "ORDER BY status_changed_at DESC LIMIT 1",
            venue_id(pv.venue), pv.canonical_pair_id, kind);
        // Lifecycle follows the EVIDENCE, it is not asserted. A band price has
        // never been rejected from is a candidate no matter how many times the
        // sweep has looked at it; only a defended band is a level.
        //   CANDIDATE  touched, never rejected  — a price, not yet a level
        //   ACTIVE     rejected at least once
        //   TESTED     rejected repeatedly       — the strongest evidence here
        const char* lifecycle = zone.reaction_count >= 2   ? "TESTED"
                                : zone.reaction_count >= 1 ? "ACTIVE"
                                                           : "CANDIDATE";
        if (existing.empty()) {
            txn.exec_params(
                "INSERT INTO alttech.venue_structure_zones (exchange_id, canonical_pair_id, "
                "timeframe, kind, lower_price, center_price, upper_price, "
                "strength_score, confidence, touch_count, reaction_count, lifecycle, "
                "algorithm_version) "
                "VALUES ($1,$2,'1m',$3,$4,$5,$6,$7,$7,$8,$9,$10,'venue-swing-2')",
                venue_id(pv.venue), pv.canonical_pair_id, kind, zone.lower, center, zone.upper,
                zone.strength, zone.touch_count, zone.reaction_count, lifecycle);
        } else {
            // Counts are ASSIGNED from the derivation, never incremented: the
            // previous `touch_count = touch_count + 1` counted sweep cycles, so
            // after four days every band reported thousands of "touches" and no
            // reactions at all, and the cross-venue consensus was building its
            // trade targets out of that.
            txn.exec_params(
                "UPDATE alttech.venue_structure_zones SET lower_price=$2, center_price=$3, "
                "upper_price=$4, strength_score=$5, confidence=$5, "
                "touch_count=$6, reaction_count=$7, lifecycle=$8, "
                // The tag has to move with the values: a row refreshed by this
                // derivation carries its numbers, so leaving the old tag on it
                // would misattribute them to the algorithm that produced the
                // cycle counter.
                "algorithm_version='venue-swing-2', "
                "last_touched_at = CASE WHEN $6 > 0 THEN now() ELSE last_touched_at END, "
                "status_changed_at=now() WHERE id=$1",
                existing[0][0].as<int64_t>(), zone.lower, center, zone.upper, zone.strength,
                zone.touch_count, zone.reaction_count, lifecycle);
        }
    });
}

} // namespace alttech::venuelite::store
