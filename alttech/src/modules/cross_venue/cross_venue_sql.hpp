#pragma once

// The cross-venue sweep's reads of AltSpotter's market tables, kept here so the
// tests exercise the statements the store actually issues.
//
// Every one of those tables is keyed (exchange_id, instrument_id, open_time).
// A read that filters only on instrument_id and time cannot use that key --
// PostgreSQL 16 has no skip scan -- so it ran as a parallel sequential scan of
// the whole month partition, through postgres_fdw, for a handful of rows:
// measured on VPS1, 358 ms for one candle minute against 0.26 ms with the venue
// list bound (Index Scan on the partition's primary key). The sweep issued
// seven such reads per pair and bar, so one pass over 27 pairs took ~140 s --
// and because next_bar_to_compute() takes the NEWEST uncomputed bar, the
// minutes in between were never computed at all: 51-52 of every 120 1m bars
// per pair got a cross-venue vector, global regime and consensus zones.
//
// Binding the four venues turns each read into four index probes. The list is
// passed as a parameter ($N = kVenueIdArray), so the key's leading column is
// always constrained and no venue can be read that the sweep does not model.

#include "alttech/domain/venue.hpp"

#include <string>

namespace alttech::crossvenue::sql {

// '{1,2,3,4}' -- the venues the cross-venue model knows, as a PostgreSQL array
// literal for `exchange_id = ANY($N::smallint[])`.
[[nodiscard]] inline std::string venue_id_array() {
    std::string out = "{";
    for (size_t i = 0; i < kAllVenues.size(); ++i) {
        if (i) out += ',';
        out += std::to_string(venue_id(kAllVenues[i]));
    }
    out += '}';
    return out;
}

// $1 canonical_pair_id  $2 bar open (ms)  $3 venue id array
inline constexpr const char* kWindowCandles =
    "SELECT exchange_id, close, revision, "
    "  (extract(epoch FROM open_time)*1000)::bigint "
    "FROM market_candles_1m WHERE exchange_id = ANY($3::smallint[]) "
    "AND instrument_id = $1 AND is_closed "
    "AND open_time = to_timestamp($2::bigint / 1000.0)";

// $1 canonical_pair_id  $2 previous bar open (ms)  $3 venue id array
inline constexpr const char* kPreviousCloses =
    "SELECT exchange_id, close FROM market_candles_1m WHERE exchange_id = ANY($3::smallint[]) "
    "AND instrument_id = $1 AND is_closed AND open_time = to_timestamp($2::bigint / 1000.0)";

// $1 canonical_pair_id  $2 bar open (ms)  $3 venue id array
inline constexpr const char* kWindowDerivatives =
    "SELECT exchange_id, mark_price, index_price, funding_rate, "
    "  open_interest_quote, best_bid, best_ask, spread, open_interest_base "
    "FROM market_derivatives_context_1m WHERE exchange_id = ANY($3::smallint[]) "
    "AND instrument_id = $1 AND open_time = to_timestamp($2::bigint / 1000.0)";

// Depth comes from the level AltSpotter actually captures. Its collectors
// sample `orderbook_depth: 15` levels and fill bid_depth_15/ask_depth_15; the
// _1/_5/_10 columns exist in the schema but no writer ever populates them, so
// reading bid_depth_5 returned NULL for every row. Prefer the shallower level
// when a deployment does record it, and fall back to 15.
//
// NULL + NULL must also stay NULL: COALESCE-ing missing depth to 0 turns "not
// measured" into "no liquidity", which the venue weighting reads as a reason to
// drop the venue entirely.
// $1 canonical_pair_id  $2 bar open (ms)  $3 venue id array
inline constexpr const char* kWindowBook =
    "SELECT exchange_id, relative_spread, "
    "  CASE "
    "    WHEN bid_depth_5 IS NOT NULL OR ask_depth_5 IS NOT NULL "
    "      THEN COALESCE(bid_depth_5,0)+COALESCE(ask_depth_5,0) "
    "    WHEN bid_depth_15 IS NOT NULL OR ask_depth_15 IS NOT NULL "
    "      THEN COALESCE(bid_depth_15,0)+COALESCE(ask_depth_15,0) "
    "  END AS depth_base, "
    "  mid_price "
    "FROM market_book_aggregates_1m WHERE exchange_id = ANY($3::smallint[]) "
    "AND instrument_id = $1 AND open_time = to_timestamp($2::bigint / 1000.0)";

// $1 canonical_pair_id  $2 bar open (ms)  $3 venue id array
inline constexpr const char* kWindowTrades =
    "SELECT exchange_id, signed_volume, last_price, trade_count "
    "FROM market_trade_aggregates_1m WHERE exchange_id = ANY($3::smallint[]) "
    "AND instrument_id = $1 AND open_time = to_timestamp($2::bigint / 1000.0)";

// $1 canonical_pair_id  $2 previous bar open (ms)  $3 venue id array
inline constexpr const char* kPreviousOpenInterest =
    "SELECT exchange_id, open_interest_quote, open_interest_base "
    "FROM market_derivatives_context_1m WHERE exchange_id = ANY($3::smallint[]) "
    "AND instrument_id = $1 AND open_time = to_timestamp($2::bigint / 1000.0)";

// $1 canonical_pair_id  $2 venue id array
inline constexpr const char* kFundingInterval =
    "SELECT exchange_id, EXTRACT(EPOCH FROM (max(funding_time) - min(funding_time))) "
    "  / NULLIF(count(*) - 1, 0) / 3600.0 AS interval_hours "
    "FROM (SELECT exchange_id, funding_time, "
    "        row_number() OVER (PARTITION BY exchange_id ORDER BY funding_time DESC) rn "
    "      FROM market_funding_settlements WHERE exchange_id = ANY($2::smallint[]) "
    "      AND instrument_id = $1) recent "
    "WHERE rn <= 8 GROUP BY exchange_id";

// $1 canonical_pair_id  $2 first bar (ms)  $3 last bar (ms)  $4 venue id array
inline constexpr const char* kReturnSeriesCloses =
    "SELECT exchange_id, (extract(epoch FROM open_time)*1000)::bigint, close "
    "FROM market_candles_1m WHERE exchange_id = ANY($4::smallint[]) "
    "AND instrument_id = $1 AND is_closed "
    "AND open_time >= to_timestamp($2::bigint / 1000.0) "
    "AND open_time <= to_timestamp($3::bigint / 1000.0) "
    "ORDER BY open_time";

// AltTech's own venue regimes, keyed (exchange_id, canonical_pair_id,
// timeframe, bar_open_time) the same way: without the venue list this was a
// sequential scan of the day's partition -- 104 594 rows filtered for 4, 34 ms,
// once per pair and pass, and growing through the day.
// $1 canonical_pair_id  $2 bar open (ms)  $3 venue id array
inline constexpr const char* kVenueRegimes =
    "SELECT exchange_id, regime, confidence, "
    "  COALESCE((axes->>'trend_strength')::float8, 0), "
    "  COALESCE((axes->>'volatility_regime')::float8, 0), "
    "  COALESCE((axes->>'liquidity_quality')::float8, 0) "
    "FROM alttech.venue_regimes WHERE exchange_id = ANY($3::smallint[]) "
    "AND canonical_pair_id = $1 AND timeframe = '1m' "
    "AND bar_open_time = to_timestamp($2::bigint / 1000.0)";

// Every statement above -- each reads a table keyed by exchange_id -- for the
// tests.
inline constexpr const char* kMarketReads[] = {
    kWindowCandles,        kPreviousCloses,       kWindowDerivatives, kWindowBook,
    kWindowTrades,         kPreviousOpenInterest, kFundingInterval,   kReturnSeriesCloses,
    kVenueRegimes,
};

} // namespace alttech::crossvenue::sql
