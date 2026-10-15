#pragma once

// market_reader — reads the price path for outcome reconstruction directly from
// AltSpotter's durable market history (read-only), so AltStrategy computes its
// own market outcomes without VPS2 (§18, §19). Path priority: raw trades → 1s
// trade aggregates → 1m candles. AltStrategy never writes these tables.

#include "altstrategy/engine/outcomes.hpp"
#include "altstrategy/engine/trade_plan.hpp"

#include <optional>
#include <string>
#include <vector>

namespace altstrategy::persistence { class PgPool; }

namespace altstrategy::engine_io {

using namespace altstrategy::engine;

struct MarketReaderConfig {
    int bitget_exchange_id = 1;                     // AltSpotter exchanges.id for Bitget
    std::string candles_table = "market_candles_1m";
    std::string trades_table = "market_trades_raw";
    bool prefer_raw_trades = true;                  // use the finest available path
    // AltSpotter's instrument registry, read for the venue's price grid at T0
    // (db/fdw/altspotter_reference.sql provisions both as foreign tables).
    std::string instrument_specs_table = "instrument_spec_versions";
    std::string pair_map_table = "canonical_pair_exchange_map";
};

struct PathResult {
    std::vector<PathBar> bars;
    OutcomePath source = OutcomePath::m1_candles;
};

class MarketReader {
public:
    MarketReader(altstrategy::persistence::PgPool& pool, MarketReaderConfig cfg)
        : pool_(pool), cfg_(std::move(cfg)) {}

    // Best-available path for `instrument_id` over [from_ms, to_ms] (the legacy
    // evaluator's read: no coverage check beyond the window's start).
    [[nodiscard]] PathResult read_path(int64_t instrument_id, int64_t from_ms, int64_t to_ms);

    // The path evaluator version 2 reads over [from_ms, to_ms), with its gaps.
    // Raw-trade buckets are used only when every minute the 1m candles show as
    // having traded also has raw trades; otherwise the 1m candles, with every
    // minute missing from them recorded as a gap. A quiet second is not a gap,
    // a minute that traded with no trade on record is.
    [[nodiscard]] PricePath read_price_path(int64_t instrument_id, int64_t from_ms, int64_t to_ms);

    // Closed Bitget 1m candles over [from_ms, to_ms], whatever finer source
    // exists. A candidate's MARKET outcome is defined on the 1m grid
    // (outcomes.hpp); read_path's 1-second trade buckets are the right path
    // for resolving a plan's stop and target, and the wrong one here.
    [[nodiscard]] std::vector<PathBar> read_minute_path(int64_t instrument_id, int64_t from_ms, int64_t to_ms) {
        return read_candles(instrument_id, from_ms, to_ms);
    }

    // Closed Bitget 1m candles in [from_ms, to_ms) as (open, quote volume,
    // base volume): the session VWAP's inputs, for rebuilding it after a
    // restart. Only quality-ok candles; a missing minute is simply absent.
    struct VolumeMinute { int64_t open_ms = 0; double quote_volume = 0, base_volume = 0; };
    [[nodiscard]] std::vector<VolumeMinute> read_volume_minutes(int64_t instrument_id,
                                                                int64_t from_ms, int64_t to_ms);

    // Every Bitget price-grid version, keyed by canonical pair. nullopt when the
    // registry could not be read -- a failed read is not an empty registry.
    [[nodiscard]] std::optional<PriceTickTable> read_price_ticks();

private:
    std::vector<PathBar> read_candles(int64_t instrument_id, int64_t from_ms, int64_t to_ms);
    std::vector<PathBar> read_trade_bars(int64_t instrument_id, int64_t from_ms, int64_t to_ms,
                                         int64_t bucket_ms);
    // Raw-trade retention is shorter than the holding horizon, so a finer source
    // must be proven to reach the START of the window before it is preferred.
    bool raw_trades_cover(int64_t instrument_id, int64_t from_ms);

    altstrategy::persistence::PgPool& pool_;
    MarketReaderConfig cfg_;
};

} // namespace altstrategy::engine_io
