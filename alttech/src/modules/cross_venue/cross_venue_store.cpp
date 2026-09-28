#include "cross_venue_store.hpp"

#include "cross_venue_features.hpp"
#include "cross_venue_sql.hpp"
#include "../venue_lite/reference_features.hpp"
#include "json_encode.hpp"

#include "alttech/persistence/sql_literal.hpp"
#include "alttech/time/time.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>

namespace alttech::crossvenue::store {

constexpr std::string_view kGlobalRegimeAlgorithm = "global-regime-1";
constexpr std::string_view kConsensusZoneAlgorithm = "consensus-zone-1";

namespace {
using persistence::sql::append_float_array;
using persistence::sql::append_bytea;


VenueQuality quality_from_text(const std::string& q) {
    if (q == "VALID") return VenueQuality::valid;
    if (q == "DEGRADED") return VenueQuality::degraded;
    if (q == "STALE") return VenueQuality::stale;
    if (q == "DESYNCED") return VenueQuality::desynced;
    return VenueQuality::not_available;
}

std::optional<double> opt_num(const pqxx::field& f) {
    if (f.is_null()) return std::nullopt;
    return f.as<double>();
}



} // namespace

Result<void> validate_schema(persistence::PgPool& pool) {
    bool ok = false;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        // All of AltTech's core output tables must exist; a missing one is a
        // schema/contract integrity breach (fail-closed).
        const auto r = txn.exec(
            "SELECT to_regclass('alttech.cross_venue_feature_vectors') IS NOT NULL "
            "   AND to_regclass('alttech.cross_venue_quality') IS NOT NULL "
            "   AND to_regclass('alttech.venue_alignment_state') IS NOT NULL "
            "   AND to_regclass('alttech.cross_venue_components') IS NOT NULL "
            "   AND to_regclass('alttech.cross_venue_snapshots') IS NOT NULL "
            "   AND to_regclass('alttech.global_regimes') IS NOT NULL "
            "   AND to_regclass('alttech.consensus_structure_zones') IS NOT NULL "
            "   AND to_regclass('alttech.bitget_projected_zones') IS NOT NULL "
            "   AND to_regclass('alttech.venue_regimes') IS NOT NULL "
            "   AND to_regclass('alttech.venue_structure_zones') IS NOT NULL "
            "   AND to_regclass('alttech.feature_set_versions') IS NOT NULL");
        ok = r[0][0].as<bool>();
    });
    if (!res) return std::unexpected(res.error());
    if (!ok) {
        return fail(ErrorCode::config_error,
                    "alttech analytics schema is missing required cross-venue tables "
                    "(run alttech-db-migrate first)");
    }
    return {};
}

Result<int64_t> ensure_cross_venue_feature_set(persistence::PgPool& pool) {
    int64_t set_id = 0;
    // Schema hash is a stable fingerprint of the ordered member names.
    std::string hash = "xv:";
    for (const auto& n : kCrossVenueFeatureNames) hash.append(n).push_back('|');

    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT id FROM alttech.feature_set_versions WHERE name = $1 AND version = $2",
            std::string(kCrossVenueSetName), kCrossVenueSetVersion);
        if (!rows.empty()) {
            set_id = rows[0][0].as<int64_t>();
            return;
        }
        const auto ins = txn.exec_params(
            "INSERT INTO alttech.feature_set_versions "
            "(name, version, scope, profile, engine_version, method_version, schema_hash, frozen) "
            "VALUES ($1, $2, 'CROSS_VENUE', 'cross_venue_core', $3, $4, $5, true) RETURNING id",
            std::string(kCrossVenueSetName), kCrossVenueSetVersion,
            std::string("alttech-xv-1"), std::string(kCrossVenueMethodVersion), hash);
        set_id = ins[0][0].as<int64_t>();
        for (size_t i = 0; i < kCrossVenueFeatureNames.size(); ++i) {
            const std::string name(kCrossVenueFeatureNames[i]);
            const auto def = txn.exec_params(
                "INSERT INTO alttech.feature_definitions (name, family, scope) "
                "VALUES ($1, 'cross_venue', 'CROSS_VENUE') "
                "ON CONFLICT (name) DO UPDATE SET family = EXCLUDED.family RETURNING id",
                name);
            const auto ps = txn.exec_params(
                "INSERT INTO alttech.feature_parameter_sets (definition_id, parameters) "
                "VALUES ($1, '{}') ON CONFLICT (definition_id, parameters) "
                "DO UPDATE SET parameters = EXCLUDED.parameters RETURNING id",
                def[0][0].as<int64_t>());
            txn.exec_params(
                "INSERT INTO alttech.feature_set_members "
                "(feature_set_version_id, position, parameter_set_id, output_name) "
                "VALUES ($1, $2, $3, $4)",
                set_id, static_cast<int>(i), ps[0][0].as<int64_t>(), name);
        }
    });
    if (!res) return std::unexpected(res.error());
    return set_id;
}

Result<std::vector<EligiblePair>> load_eligible_universe(persistence::PgPool& pool) {
    std::vector<EligiblePair> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto pairs = txn.exec(
            "SELECT u.canonical_pair_id, cp.base_asset, cp.quote_asset "
            "FROM global_observation_universe u "
            "JOIN canonical_pairs cp ON cp.id = u.canonical_pair_id "
            "WHERE u.eligible ORDER BY u.canonical_pair_id");
        for (const auto& p : pairs) {
            EligiblePair ep;
            ep.canonical_pair_id = p[0].as<int64_t>();
            ep.base_asset = p[1].as<std::string>();
            ep.quote_asset = p[2].as<std::string>();
            const auto venues = txn.exec_params(
                "SELECT m.exchange_id, ei.exchange_symbol "
                "FROM canonical_pair_exchange_map m "
                "JOIN exchange_instruments ei ON ei.id = m.exchange_instrument_id "
                "WHERE m.canonical_pair_id = $1 AND ei.is_whitelisted "
                "AND ei.status = 'active' ORDER BY m.exchange_id",
                ep.canonical_pair_id);
            for (const auto& v : venues) {
                const auto venue = venue_from_id(v[0].as<int>());
                if (!venue) continue;
                EligiblePair::VenueRef ref;
                ref.venue = *venue;
                ref.instrument_id = ep.canonical_pair_id; // shared canonical id
                ref.exchange_symbol = v[1].as<std::string>();
                ep.venues.push_back(ref);
            }
            out.push_back(std::move(ep));
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<std::optional<int64_t>> next_bar_to_compute(persistence::PgPool& pool,
                                                   const EligiblePair& pair, int64_t watermark_ms,
                                                   int lookback_min) {
    std::optional<int64_t> out;
    const int64_t now = timeutil::now_ms();
    const int64_t horizon = now - static_cast<int64_t>(lookback_min) * 60'000;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        // Newest closed Bitget 1m bar past the watermark that AltTech has not yet
        // produced a cross-venue vector for.
        const auto r = txn.exec_params(
            "SELECT (extract(epoch FROM c.open_time) * 1000)::bigint "
            "FROM market_candles_1m c "
            "WHERE c.exchange_id = 1 AND c.instrument_id = $1 AND c.is_closed "
            "  AND c.open_time <= to_timestamp($2::bigint / 1000.0) "
            "  AND c.open_time >= to_timestamp($3::bigint / 1000.0) "
            "  AND NOT EXISTS (SELECT 1 FROM alttech.cross_venue_feature_vectors v "
            "                  WHERE v.canonical_pair_id = $1 AND v.timeframe = '1m' "
            "                  AND v.bar_open_time = c.open_time) "
            "ORDER BY c.open_time DESC LIMIT 1",
            pair.canonical_pair_id, now - watermark_ms, horizon);
        if (!r.empty() && !r[0][0].is_null()) out = r[0][0].as<int64_t>();
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<std::vector<VenueSample>> load_window(persistence::PgPool& pool, const EligiblePair& pair,
                                             int64_t bar_open_ms) {
    std::map<Venue, VenueSample> byv;
    for (const auto v : kAllVenues) {
        VenueSample s;
        s.venue = v;
        byv[v] = s;
    }
    auto res = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        const std::string ts = "to_timestamp(" + std::to_string(bar_open_ms) + "::bigint / 1000.0)";

        // Every read below binds the venue list: see cross_venue_sql.hpp for
        // what an unbound read costs on a partitioned market table.
        const std::string venues = sql::venue_id_array();
        // Candles: close (mid proxy), previous close (log return), revision.
        for (const auto& row : txn.exec_params(sql::kWindowCandles, pair.canonical_pair_id,
                                               bar_open_ms, venues)) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            auto& s = byv[*venue];
            s.present = true;
            s.mid_price = row[1].as<double>();
            s.bar_revision = row[2].as<int64_t>();
        }
        // Previous-minute close for log returns.
        for (const auto& row : txn.exec_params(sql::kPreviousCloses, pair.canonical_pair_id,
                                               bar_open_ms - 60'000, venues)) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            auto& s = byv[*venue];
            const double prev = row[1].as<double>();
            if (s.mid_price && prev > 0.0 && *s.mid_price > 0.0) {
                s.log_return = std::log(*s.mid_price / prev);
            }
        }
        // Derivatives context: mark/index/funding/OI/spread.
        for (const auto& row : txn.exec_params(sql::kWindowDerivatives, pair.canonical_pair_id,
                                               bar_open_ms, venues)) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            auto& s = byv[*venue];
            s.mark_price = opt_num(row[1]);
            s.index_price = opt_num(row[2]);
            if (const auto fr = opt_num(row[3])) s.funding_rate = fr;
            s.oi_usd = opt_num(row[4]);
            s.oi_base = opt_num(row[8]);
        }
        // Book aggregates: relative spread, depth, microprice. Depth semantics
        // are explained with the statement (cross_venue_sql.hpp).
        for (const auto& row : txn.exec_params(sql::kWindowBook, pair.canonical_pair_id,
                                               bar_open_ms, venues)) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            auto& s = byv[*venue];
            s.relative_spread = opt_num(row[1]);
            // Convert base-unit depth to USDT notional with the venue mid price.
            if (const auto d = opt_num(row[2]); d && s.mid_price) {
                s.depth_usdt = *d * *s.mid_price;
            }
            if (!s.mid_price) s.mid_price = opt_num(row[3]);
        }

        // Per-venue technical state. Both sources are analytics AltTech already
        // produces for every venue on the 1m grid and, until now, nothing read:
        // the primary venue's curated projection, and the reference_lite vectors
        // for the other three. Reading them here is what lets the consensus
        // agree (or disagree) on the technical picture, not just on price.
        for (const auto& row : txn.exec_params(
                 "SELECT exchange_id, rsi_14, atr_14, realized_vol_20, ema_20, ema_50, ema_200 "
                 "FROM alttech.venue_feature_projection "
                 "WHERE canonical_pair_id = $1 AND timeframe = '1m' "
                 "AND bar_open_time = to_timestamp($2::bigint / 1000.0)",
                 pair.canonical_pair_id, bar_open_ms)) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            auto& s = byv[*venue];
            s.rsi_14 = opt_num(row[1]);
            s.atr_14 = opt_num(row[2]);
            s.realized_vol_20 = opt_num(row[3]);
            // Stacked EMAs: +1 fully bullish, -1 fully bearish, 0 mixed. The
            // projection stores the levels, so the stacking is derived here the
            // same way reference_lite derives its own.
            const auto e20 = opt_num(row[4]);
            const auto e50 = opt_num(row[5]);
            const auto e200 = opt_num(row[6]);
            if (e20 && e50 && e200) {
                s.trend_alignment = (*e20 > *e50 && *e50 > *e200)   ?  1.0
                                    : (*e20 < *e50 && *e50 < *e200) ? -1.0
                                                                    :  0.0;
            }
        }
        for (const auto& row : txn.exec_params(
                 "SELECT exchange_id, values[$3], values[$4], values[$5], values[$6] "
                 "FROM alttech.venue_feature_vectors_reference "
                 "WHERE canonical_pair_id = $1 AND timeframe = '1m' "
                 "AND bar_open_time = to_timestamp($2::bigint / 1000.0) "
                 "AND status = 'VALID' "
                 // $3..$6 are indices into the reference_lite LAYOUT. This
                 // partition also carries full TA-Lib vectors now that every
                 // venue is computed, and at those same indices that layout
                 // holds entirely different features -- reading one as the
                 // other would feed the cross-venue consensus silent nonsense
                 // rather than an error. Pin the engine that defines the layout.
                 "AND engine_version = $7",
                 pair.canonical_pair_id, bar_open_ms,
                 // PostgreSQL arrays are 1-based; the enum is 0-based.
                 static_cast<int>(venuelite::RF_RSI_14) + 1,
                 static_cast<int>(venuelite::RF_ATR_14) + 1,
                 static_cast<int>(venuelite::RF_REALIZED_VOL_20) + 1,
                 static_cast<int>(venuelite::RF_EMA_ALIGNMENT) + 1,
                 std::string(venuelite::kReferenceEngine))) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            auto& s = byv[*venue];
            if (!s.rsi_14) s.rsi_14 = opt_num(row[1]);
            if (!s.atr_14) s.atr_14 = opt_num(row[2]);
            if (!s.realized_vol_20) s.realized_vol_20 = opt_num(row[3]);
            if (!s.trend_alignment) s.trend_alignment = opt_num(row[4]);
        }

        // Trade aggregates: signed volume, trade intensity.
        for (const auto& row : txn.exec_params(sql::kWindowTrades, pair.canonical_pair_id,
                                               bar_open_ms, venues)) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            auto& s = byv[*venue];
            const auto price = opt_num(row[2]).value_or(s.mid_price.value_or(0.0));
            if (const auto sv = opt_num(row[1]); sv && price > 0.0) {
                s.signed_volume_usdt = *sv * price;
            }
            s.trade_intensity = opt_num(row[3]);
        }
        // Previous-minute open interest -> relative change. OI is
        // never guessed: absent previous OI leaves the change unavailable.
        //
        // Both OI columns are read because only ONE of them is populated on any
        // given venue: measured over 30 minutes, `open_interest_quote` covered
        // 100% of Bybit rows and 58% of OKX but 0% of Bitget and Binance, while
        // `open_interest_base` covered 96-100% everywhere. Reading quote alone
        // left the whole OI block blind to the execution venue, so
        // `bitget_oi_deviation` was invalid in 224 of 224 vectors and the
        // "consensus" was built from two venues out of four.
        //
        // The ratio is unit-invariant only if both ends come from the same
        // column, so the pair is chosen consistently rather than coalesced.
        for (const auto& row : txn.exec_params(sql::kPreviousOpenInterest,
                                               pair.canonical_pair_id, bar_open_ms - 60'000,
                                               venues)) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            auto& s = byv[*venue];
            const auto prev_quote = opt_num(row[1]);
            const auto prev_base = opt_num(row[2]);
            if (prev_quote && *prev_quote > 0.0 && s.oi_usd && *s.oi_usd >= 0.0) {
                s.oi_change_ratio = (*s.oi_usd - *prev_quote) / *prev_quote;
            } else if (prev_base && *prev_base > 0.0 && s.oi_base && *s.oi_base >= 0.0) {
                s.oi_change_ratio = (*s.oi_base - *prev_base) / *prev_base;
            }
        }

        // Funding settlement interval, measured per venue from that venue's own
        // settlement history. Venues do not share an interval and it is never
        // assumed: without history the funding features stay unavailable.
        for (const auto& row : txn.exec_params(sql::kFundingInterval, pair.canonical_pair_id,
                                               venues)) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            const auto hours = opt_num(row[1]);
            if (hours && *hours > 0.0) byv[*venue].funding_interval_hours = hours;
        }

        // Live quality + snapshot age from AltSpotter exchange_data_quality.
        for (const auto& row : txn.exec_params(
                 "SELECT exchange_id, quality, COALESCE(candle_age_sec,0), "
                 "  COALESCE(sequence_gap_count,0) "
                 "FROM exchange_data_quality WHERE instrument_id = $1",
                 pair.canonical_pair_id)) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            auto& s = byv[*venue];
            s.quality = quality_from_text(row[1].as<std::string>());
            s.data_age_ms = static_cast<double>(row[2].as<int64_t>()) * 1000.0;
        }
        // Per-venue clock offset (EWMA).
        for (const auto& row : txn.exec("SELECT exchange_id, offset_ms FROM exchange_clock_offsets")) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            byv[*venue].clock_offset_ms = static_cast<double>(row[1].as<int64_t>());
        }
    });
    if (!res) return std::unexpected(res.error());

    // A venue that delivered no bar is NOT_AVAILABLE; a venue that delivered one
    // but has no quality row is VALID, because delivering the bar is itself the
    // evidence. Absence of a measured data age is likewise absence of evidence
    // of staleness, not evidence of it: it stays 0.
    std::vector<VenueSample> samples;
    for (const auto v : kAllVenues) {
        auto& s = byv[v];
        if (s.present && s.quality == VenueQuality::not_available) s.quality = VenueQuality::valid;
        if (s.data_age_ms < 0.0) s.data_age_ms = 0.0;
        samples.push_back(s);
    }
    return samples;
}

Result<std::vector<ReturnSeries>> load_return_series(persistence::PgPool& pool,
                                                     const EligiblePair& pair, int64_t end_bar_ms,
                                                     int count) {
    count = std::clamp(count, 2, 512);
    // Grid = 1 MINUTE, and that is a real limitation, not a choice: the book is
    // polled every 5 s for core instruments and 15 s otherwise, so
    // market_book_aggregates_1s holds roughly one bucket per 5.5 s. There is no
    // second-resolution series in the system to build a finer grid from, and a
    // 1 s grid over 5 s samples produces mostly-zero returns whose phase differs
    // per venue, which destroys the correlation outright (measured: confidence
    // fell from ~0.004 to exactly 0). Resolving true cross-venue leadership needs
    // a faster book poll in AltSpotter first; until then this measures only what
    // a minute grid can, and lead_confidence should be read as weak evidence.
    const int64_t span = static_cast<int64_t>(count) * 60'000;
    const int64_t start_ms = end_bar_ms - span;
    std::map<Venue, std::map<int64_t, double>> closes;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        for (const auto& row : txn.exec_params(sql::kReturnSeriesCloses, pair.canonical_pair_id,
                                               start_ms, end_bar_ms, sql::venue_id_array())) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            closes[*venue][row[1].as<int64_t>()] = row[2].as<double>();
        }
    });
    if (!res) return std::unexpected(res.error());

    std::vector<ReturnSeries> out;
    for (const auto v : kAllVenues) {
        ReturnSeries rs;
        rs.venue = v;
        rs.returns.reserve(static_cast<std::size_t>(count));
        const auto& m = closes[v];
        for (int i = 0; i < count; ++i) {
            const int64_t t = start_ms + static_cast<int64_t>(i + 1) * 60'000;
            const auto cur = m.find(t);
            const auto prev = m.find(t - 60'000);
            double r = 0.0; // aligned grid: missing minute contributes a 0 return
            if (cur != m.end() && prev != m.end() && prev->second > 0.0 && cur->second > 0.0) {
                r = std::log(cur->second / prev->second);
            }
            rs.returns.push_back(r);
        }
        out.push_back(std::move(rs));
    }
    return out;
}

Result<void> write_cross_venue(persistence::PgPool& pool, const CrossVenueRecord& rec) {
    return pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        const std::string ts =
            "to_timestamp(" + std::to_string(rec.bar_open_ms) + "::bigint / 1000.0)";
        const std::string close_ts =
            "to_timestamp(" + std::to_string(rec.canonical_close_ms) + "::bigint / 1000.0)";

        // 1) cross_venue_feature_vectors
        {
            std::string sql =
                "INSERT INTO alttech.cross_venue_feature_vectors (canonical_pair_id, timeframe, "
                "bar_open_time, feature_set_version_id, method_version, values, "
                "validity_bitmap, status, quality_status, venue_mask, missing_venue_mask, "
                "stale_venue_mask, desynced_venue_mask, confidence, computed_at) VALUES (";
            sql += std::to_string(rec.canonical_pair_id) + ",'1m'," + ts + "," +
                   std::to_string(rec.feature_set_id) + "," + txn.quote(kCrossVenueMethodVersion) +
                   ",";
            append_float_array(sql, rec.values);
            sql += ",";
            append_bytea(sql, rec.validity);
            const std::string status =
                rec.quality == CrossVenueQuality::invalid ? "INVALID" : "VALID";
            sql += "," + txn.quote(status) + "," + txn.quote(std::string(quality_label(rec.quality))) +
                   "," + std::to_string(rec.venue_mask) + "," +
                   std::to_string(rec.missing_venue_mask) + "," +
                   std::to_string(rec.stale_venue_mask) + "," +
                   std::to_string(rec.desynced_venue_mask) + "," +
                   std::to_string(rec.confidence) + ",to_timestamp(" +
                   std::to_string(rec.computed_at_ms) + "::bigint / 1000.0))";
            sql +=
                " ON CONFLICT (canonical_pair_id, timeframe, bar_open_time, "
                "feature_set_version_id) DO UPDATE SET values = EXCLUDED.values, "
                "validity_bitmap = EXCLUDED.validity_bitmap, status = EXCLUDED.status, "
                "quality_status = EXCLUDED.quality_status, venue_mask = EXCLUDED.venue_mask, "
                "missing_venue_mask = EXCLUDED.missing_venue_mask, "
                "stale_venue_mask = EXCLUDED.stale_venue_mask, "
                "desynced_venue_mask = EXCLUDED.desynced_venue_mask, "
                "confidence = EXCLUDED.confidence, computed_at = EXCLUDED.computed_at, "
                "updated_at = now()";
            txn.exec(sql);
        }

        // 2) cross_venue_quality
        {
            const std::string reasons = json_string_array(rec.quality_reasons);
            txn.exec_params(
                "INSERT INTO alttech.cross_venue_quality (canonical_pair_id, timeframe, "
                "bar_open_time, quality_status, venue_mask, missing_venue_mask, stale_venue_mask, "
                "desynced_venue_mask, confidence, quality_reasons, observed_at) "
                "VALUES ($1, '1m', to_timestamp($2::bigint / 1000.0), $3, $4, $5, $6, $7, $8, "
                "$9::jsonb, now()) "
                "ON CONFLICT (canonical_pair_id, timeframe, bar_open_time) DO UPDATE SET "
                "quality_status = EXCLUDED.quality_status, venue_mask = EXCLUDED.venue_mask, "
                "missing_venue_mask = EXCLUDED.missing_venue_mask, "
                "stale_venue_mask = EXCLUDED.stale_venue_mask, "
                "desynced_venue_mask = EXCLUDED.desynced_venue_mask, "
                "confidence = EXCLUDED.confidence, quality_reasons = EXCLUDED.quality_reasons, "
                "observed_at = now()",
                rec.canonical_pair_id, rec.bar_open_ms,
                std::string(quality_label(rec.quality)), static_cast<int>(rec.venue_mask),
                static_cast<int>(rec.missing_venue_mask), static_cast<int>(rec.stale_venue_mask),
                static_cast<int>(rec.desynced_venue_mask), rec.confidence, reasons);
        }

        // 3) venue_alignment_state
        txn.exec_params(
            "INSERT INTO alttech.venue_alignment_state (canonical_pair_id, timeframe, "
            "canonical_open_time, canonical_close_time, watermark_time, venue_mask, "
            "missing_venue_mask, late_venue_mask, revised_venue_mask, alignment_status, "
            "max_cross_venue_skew_ms, observed_at) "
            "VALUES ($1, '1m', to_timestamp($2::bigint / 1000.0), to_timestamp($3::bigint / 1000.0), "
            "now(), $4, $5, 0, 0, $6, $7, now()) "
            "ON CONFLICT (canonical_pair_id, timeframe, canonical_open_time) DO UPDATE SET "
            "venue_mask = EXCLUDED.venue_mask, missing_venue_mask = EXCLUDED.missing_venue_mask, "
            "alignment_status = EXCLUDED.alignment_status, "
            "max_cross_venue_skew_ms = EXCLUDED.max_cross_venue_skew_ms, observed_at = now()",
            rec.canonical_pair_id, rec.bar_open_ms, rec.canonical_close_ms,
            static_cast<int>(rec.venue_mask), static_cast<int>(rec.missing_venue_mask),
            std::string(alignment_label(rec.alignment)), rec.max_skew_ms);

        // 4) cross_venue_components (one row per venue, reproducibility)
        for (const auto& c : rec.contributions) {
            if (c.venue == Venue::unknown) continue;
            txn.exec_params(
                "INSERT INTO alttech.cross_venue_components (canonical_pair_id, timeframe, "
                "bar_open_time, exchange_id, weight, included, freshness_ms, "
                "relative_spread, depth_usdt, venue_quality, exclude_reason) "
                "VALUES ($1, '1m', to_timestamp($2::bigint / 1000.0), $3, $4, $5, $6, $7, $8, "
                "$9, $10) "
                "ON CONFLICT (canonical_pair_id, timeframe, bar_open_time, "
                "exchange_id) DO UPDATE SET weight = EXCLUDED.weight, included = EXCLUDED.included, "
                "freshness_ms = EXCLUDED.freshness_ms, relative_spread = EXCLUDED.relative_spread, "
                "depth_usdt = EXCLUDED.depth_usdt, venue_quality = EXCLUDED.venue_quality, "
                "exclude_reason = EXCLUDED.exclude_reason",
                rec.canonical_pair_id, rec.bar_open_ms, venue_id(c.venue), c.weight, c.included,
                c.freshness_ms, c.relative_spread, c.depth_usdt,
                std::string(quality_label(c.quality)), c.exclude_reason);
        }

        // 5) cross_venue_snapshots (compact human/consumer view)
        txn.exec_params(
            "INSERT INTO alttech.cross_venue_snapshots (canonical_pair_id, timeframe, "
            "bar_open_time, consensus_mid_price, consensus_mark_price, "
            "consensus_index_price, bitget_mid_deviation_bps, venue_price_dispersion, "
            "dominant_leading_venue, lead_confidence, quality_status, venue_mask, computed_at) "
            "VALUES ($1, '1m', to_timestamp($2::bigint / 1000.0), $3, $4, $5, $6, $7, $8, $9, "
            "$10, $11, now()) "
            "ON CONFLICT (canonical_pair_id, timeframe, bar_open_time) "
            "DO UPDATE SET consensus_mid_price = EXCLUDED.consensus_mid_price, "
            "consensus_mark_price = EXCLUDED.consensus_mark_price, "
            "consensus_index_price = EXCLUDED.consensus_index_price, "
            "bitget_mid_deviation_bps = EXCLUDED.bitget_mid_deviation_bps, "
            "venue_price_dispersion = EXCLUDED.venue_price_dispersion, "
            "dominant_leading_venue = EXCLUDED.dominant_leading_venue, "
            "lead_confidence = EXCLUDED.lead_confidence, quality_status = EXCLUDED.quality_status, "
            "venue_mask = EXCLUDED.venue_mask, computed_at = now()",
            rec.canonical_pair_id, rec.bar_open_ms,
            rec.consensus_mid ? std::optional<double>(*rec.consensus_mid) : std::nullopt,
            rec.consensus_mark ? std::optional<double>(*rec.consensus_mark) : std::nullopt,
            rec.consensus_index ? std::optional<double>(*rec.consensus_index) : std::nullopt,
            rec.bitget_mid_dev_bps ? std::optional<double>(*rec.bitget_mid_dev_bps) : std::nullopt,
            rec.venue_price_dispersion, venue_id(rec.dominant_leading_venue), rec.lead_confidence,
            std::string(quality_label(rec.quality)), static_cast<int>(rec.venue_mask));
    });
}


Result<std::vector<VenueRegimeInput>> load_venue_regimes(persistence::PgPool& pool,
                                                         int64_t canonical_pair_id,
                                                         int64_t bar_open_ms) {
    std::vector<VenueRegimeInput> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        for (const auto& row : txn.exec_params(sql::kVenueRegimes, canonical_pair_id,
                                               bar_open_ms, sql::venue_id_array())) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            VenueRegimeInput in;
            in.venue = *venue;
            in.regime = row[1].as<std::string>();
            in.confidence = row[2].as<double>();
            in.trend_strength = row[3].as<double>();
            in.volatility_regime = row[4].as<double>();
            in.liquidity_quality = row[5].as<double>();
            out.push_back(std::move(in));
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<std::vector<VenueZoneInput>> load_venue_zones(persistence::PgPool& pool,
                                                     int64_t canonical_pair_id) {
    std::vector<VenueZoneInput> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        for (const auto& row : txn.exec_params(
                 "SELECT exchange_id, kind, lower_price::float8, center_price::float8, "
                 "  upper_price::float8, strength_score "
                 // CANDIDATE is deliberately excluded. A candidate is a band
                 // price has entered but never been rejected from — a price, not
                 // a level — and these zones become trade TARGETS downstream via
                 // the consensus projection. Requiring a rejection is the whole
                 // difference between "a line the market defended" and "a line
                 // the market happens to be near".
                 "FROM alttech.venue_structure_zones WHERE canonical_pair_id = $1 "
                 "AND timeframe = '1m' AND lifecycle IN ('ACTIVE','TESTED') "
                 "AND reaction_count >= 1",
                 canonical_pair_id)) {
            const auto venue = venue_from_id(row[0].as<int>());
            if (!venue) continue;
            VenueZoneInput z;
            z.venue = *venue;
            z.kind = row[1].as<std::string>();
            z.lower = row[2].as<double>();
            z.center = row[3].as<double>();
            z.upper = row[4].as<double>();
            z.strength = row[5].as<double>();
            out.push_back(std::move(z));
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

Result<void> write_global_regime(persistence::PgPool& pool, int64_t canonical_pair_id,
                                 int64_t bar_open_ms, const GlobalRegimeResult& regime,
                                 CrossVenueQuality quality) {
    if (!regime.available) return {};
    return pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL TIME ZONE 'UTC'");
        // The previous regime, for the transition event.
        const auto previous = txn.exec_params(
            "SELECT primary_regime FROM alttech.global_regimes WHERE canonical_pair_id = $1 "
            "AND timeframe = '1m' AND bar_open_time < to_timestamp($2::bigint / 1000.0) "
            "ORDER BY bar_open_time DESC LIMIT 1",
            canonical_pair_id, bar_open_ms);
        txn.exec_params(
            "INSERT INTO alttech.global_regimes (canonical_pair_id, timeframe, bar_open_time, "
            "primary_regime, regime_confidence, venue_agreement, venue_dispersion, "
            "supporting_venue_mask, bitget_regime_divergence, trend_consensus, "
            "volatility_consensus, liquidity_consensus, quality_status, algorithm_version) "
            "VALUES ($1, '1m', to_timestamp($2::bigint / 1000.0), $3, $4, $5, $6, $7, $8, $9, "
            "$10, $11, $12, $13) "
            "ON CONFLICT (canonical_pair_id, timeframe, bar_open_time) DO UPDATE SET "
            "primary_regime = EXCLUDED.primary_regime, "
            "regime_confidence = EXCLUDED.regime_confidence, "
            "venue_agreement = EXCLUDED.venue_agreement, "
            "venue_dispersion = EXCLUDED.venue_dispersion, "
            "supporting_venue_mask = EXCLUDED.supporting_venue_mask, "
            "bitget_regime_divergence = EXCLUDED.bitget_regime_divergence, "
            "trend_consensus = EXCLUDED.trend_consensus, "
            "volatility_consensus = EXCLUDED.volatility_consensus, "
            "liquidity_consensus = EXCLUDED.liquidity_consensus, "
            "quality_status = EXCLUDED.quality_status, computed_at = now()",
            canonical_pair_id, bar_open_ms, regime.primary_regime, regime.regime_confidence,
            regime.venue_agreement, regime.venue_dispersion,
            static_cast<int>(regime.supporting_venue_mask), regime.bitget_divergence,
            regime.trend_consensus, regime.volatility_consensus, regime.liquidity_consensus,
            std::string(quality_label(quality)), std::string(kGlobalRegimeAlgorithm));
        if (!previous.empty()) {
            const std::string from = previous[0][0].as<std::string>();
            if (from != regime.primary_regime) {
                txn.exec_params(
                    "INSERT INTO alttech.regime_events (scope, canonical_pair_id, timeframe, "
                    "from_regime, to_regime, bar_open_time) "
                    "VALUES ('CROSS_VENUE', $1, '1m', $2, $3, "
                    "to_timestamp($4::bigint / 1000.0))",
                    canonical_pair_id, from, regime.primary_regime, bar_open_ms);
            }
        }
    });
}

Result<void> write_consensus_zones(persistence::PgPool& pool, int64_t canonical_pair_id,
                                   std::span<const ConsensusZone> zones, double primary_mid,
                                   double consensus_mid, double window_confidence,
                                   CrossVenueQuality quality, int64_t stale_after_sec) {
    return pool.with_txn([&](pqxx::work& txn) {
        for (const auto& zone : zones) {
            const double half = std::abs(zone.normalized_width) / 2.0;
            const double lower = zone.normalized_center - half;
            const double upper = zone.normalized_center + half;
            // A zone is "the same level" when its normalized band overlaps an
            // existing one of the same kind, so a level that drifts with the
            // market keeps its identity and its lifecycle.
            const auto existing = txn.exec_params(
                "SELECT id FROM alttech.consensus_structure_zones "
                "WHERE canonical_pair_id = $1 AND timeframe = '1m' AND kind = $2 "
                "AND lifecycle NOT IN ('BROKEN','EXPIRED','FLIPPED') "
                "AND normalized_center - normalized_width / 2 <= $4 "
                "AND normalized_center + normalized_width / 2 >= $3 "
                "ORDER BY abs(normalized_center - $5) LIMIT 1",
                canonical_pair_id, zone.kind, lower, upper, zone.normalized_center);

            const std::string lifecycle = zone.supporting_venue_count >= 3 ? "ACTIVE" : "CANDIDATE";
            int64_t zone_id = 0;
            if (existing.empty()) {
                const auto inserted = txn.exec_params(
                    "INSERT INTO alttech.consensus_structure_zones (canonical_pair_id, timeframe, "
                    "kind, normalized_center, normalized_width, supporting_venue_mask, "
                    "supporting_venue_count, venue_confluence_score, strength_score, confidence, "
                    "lifecycle, quality_status, algorithm_version) "
                    "VALUES ($1, '1m', $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12) RETURNING id",
                    canonical_pair_id, zone.kind, zone.normalized_center, zone.normalized_width,
                    static_cast<int>(zone.supporting_venue_mask), zone.supporting_venue_count,
                    zone.venue_confluence_score, zone.strength, zone.confidence, lifecycle,
                    std::string(quality_label(quality)), std::string(kConsensusZoneAlgorithm));
                zone_id = inserted[0][0].as<int64_t>();
                txn.exec_params(
                    "INSERT INTO alttech.structure_zone_events (scope, canonical_pair_id, "
                    "zone_id, timeframe, event) VALUES ('CROSS_VENUE', $1, $2, '1m', 'created')",
                    canonical_pair_id, zone_id);
            } else {
                zone_id = existing[0][0].as<int64_t>();
                txn.exec_params(
                    "UPDATE alttech.consensus_structure_zones SET normalized_center = $2, "
                    "normalized_width = $3, supporting_venue_mask = $4, "
                    "supporting_venue_count = $5, venue_confluence_score = $6, "
                    "strength_score = $7, confidence = $8, lifecycle = $9, quality_status = $10, "
                    "status_changed_at = now() WHERE id = $1",
                    zone_id, zone.normalized_center, zone.normalized_width,
                    static_cast<int>(zone.supporting_venue_mask), zone.supporting_venue_count,
                    zone.venue_confluence_score, zone.strength, zone.confidence, lifecycle,
                    std::string(quality_label(quality)));
            }

            const auto projected =
                project_to_primary(zone, primary_mid, consensus_mid, window_confidence);
            if (projected.center > 0.0) {
                txn.exec_params(
                    "INSERT INTO alttech.bitget_projected_zones (canonical_pair_id, timeframe, "
                    "source_consensus_zone_id, bitget_basis_adjustment, projected_lower, "
                    "projected_center, projected_upper, projection_quality, algorithm_version) "
                    "VALUES ($1, '1m', $2, $3, $4, $5, $6, $7, $8) "
                    "ON CONFLICT (source_consensus_zone_id) DO UPDATE SET "
                    "bitget_basis_adjustment = EXCLUDED.bitget_basis_adjustment, "
                    "projected_lower = EXCLUDED.projected_lower, "
                    "projected_center = EXCLUDED.projected_center, "
                    "projected_upper = EXCLUDED.projected_upper, "
                    "projection_quality = EXCLUDED.projection_quality, "
                    "projection_time = now()",
                    canonical_pair_id, zone_id, projected.basis_adjustment, projected.lower,
                    projected.center, projected.upper, projected.quality,
                    std::string(kConsensusZoneAlgorithm));
            }
        }

        // A consensus zone no venue has confirmed for a while has stopped being
        // a level the market recognizes.
        txn.exec_params(
            "UPDATE alttech.consensus_structure_zones SET lifecycle = 'EXPIRED', "
            "status_changed_at = now() WHERE canonical_pair_id = $1 AND timeframe = '1m' "
            "AND lifecycle NOT IN ('BROKEN','EXPIRED','FLIPPED') "
            "AND status_changed_at < now() - make_interval(secs => $2)",
            canonical_pair_id, static_cast<double>(stale_after_sec));
    });
}

} // namespace alttech::crossvenue::store
