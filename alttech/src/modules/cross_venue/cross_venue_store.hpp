#pragma once
// PostgreSQL access for the cross_venue module. Reads AltSpotter market-data
// tables read-only and writes ONLY the alttech schema.

#include "analytics_types.hpp"
#include "consensus.hpp"
#include "global_view.hpp"

#include "alttech/persistence/pg.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alttech::crossvenue::store {

// One eligible canonical pair plus its per-venue instrument realization.
struct EligiblePair {
    int64_t canonical_pair_id = 0;
    std::string base_asset;
    std::string quote_asset;
    struct VenueRef {
        Venue venue = Venue::unknown;
        int64_t instrument_id = 0;   // exchange_instruments.instrument_id
        std::string exchange_symbol;
    };
    std::vector<VenueRef> venues;
};

// Fail-closed check that the AltTech-owned schema and its core tables exist.
[[nodiscard]] Result<void> validate_schema(persistence::PgPool& pool);

// Register (or verify) the cross_venue_core feature set; returns its id.
[[nodiscard]] Result<int64_t> ensure_cross_venue_feature_set(persistence::PgPool& pool);

// Current globally eligible universe (four-venue intersection) with per-venue
// symbols/instrument ids.
[[nodiscard]] Result<std::vector<EligiblePair>> load_eligible_universe(persistence::PgPool& pool);

// Newest closed 1m bar_open_time (ms) for a pair for which Bitget has a candle,
// old enough to have passed the watermark, and not yet in the cross-venue store.
// nullopt when there is nothing to compute.
[[nodiscard]] Result<std::optional<int64_t>> next_bar_to_compute(
    persistence::PgPool& pool, const EligiblePair& pair, int64_t watermark_ms, int lookback_min);

// Assemble the aligned per-venue samples for one 1m window from AltSpotter's
// durable candles/context/aggregates and operational quality tables.
[[nodiscard]] Result<std::vector<VenueSample>> load_window(persistence::PgPool& pool,
                                                           const EligiblePair& pair,
                                                           int64_t bar_open_ms);

// Bounded per-venue log-return series on the aligned 1m grid ending at
// `end_bar_ms` (inclusive), newest last, `count` samples. Venues share the grid;
// a missing minute for a venue yields a 0 return so the series stay aligned.
struct ReturnSeries {
    Venue venue = Venue::unknown;
    std::vector<double> returns;
};
[[nodiscard]] Result<std::vector<ReturnSeries>> load_return_series(persistence::PgPool& pool,
                                                                   const EligiblePair& pair,
                                                                   int64_t end_bar_ms, int count);

// The persisted cross-venue result for one window.
struct CrossVenueRecord {
    int64_t canonical_pair_id = 0;
    std::string timeframe = "1m";
    int64_t bar_open_ms = 0;
    int64_t feature_set_id = 0;
    std::vector<float> values;          // XV_FEATURE_COUNT wide
    std::vector<uint8_t> validity;      // bitmap
    CrossVenueQuality quality = CrossVenueQuality::invalid;
    AlignmentStatus alignment = AlignmentStatus::invalid;
    uint32_t venue_mask = 0;
    uint32_t missing_venue_mask = 0;
    uint32_t stale_venue_mask = 0;
    uint32_t desynced_venue_mask = 0;
    double confidence = 0.0;
    double max_skew_ms = 0.0;
    int64_t canonical_close_ms = 0;
    std::optional<double> consensus_mid;
    std::optional<double> consensus_mark;
    std::optional<double> consensus_index;
    std::optional<double> bitget_mid_dev_bps;
    double venue_price_dispersion = 0.0;
    Venue dominant_leading_venue = Venue::unknown;
    double lead_confidence = 0.0;
    std::vector<Contribution> contributions;
    std::vector<std::string> quality_reasons;
    int64_t computed_at_ms = 0;
};

// Idempotent upsert of the cross-venue feature vector, quality, alignment,
// per-venue components and the compact snapshot for one window.
[[nodiscard]] Result<void> write_cross_venue(persistence::PgPool& pool,
                                             const CrossVenueRecord& rec);

// The per-venue 1m regimes the venue sweeps stored for one bar. `weight` is
// left at zero for the caller to fill from the consensus contributions.
[[nodiscard]] Result<std::vector<VenueRegimeInput>> load_venue_regimes(
    persistence::PgPool& pool, int64_t canonical_pair_id, int64_t bar_open_ms);

// The per-venue 1m structure zones currently alive. `venue_mid` is left at zero
// for the caller to fill from the aligned window samples.
[[nodiscard]] Result<std::vector<VenueZoneInput>> load_venue_zones(persistence::PgPool& pool,
                                                                   int64_t canonical_pair_id);

[[nodiscard]] Result<void> write_global_regime(persistence::PgPool& pool,
                                               int64_t canonical_pair_id, int64_t bar_open_ms,
                                               const GlobalRegimeResult& regime,
                                               CrossVenueQuality quality);

// Upsert the consensus zone set and its projection onto the primary venue, and
// expire consensus zones that no venue has confirmed for `stale_after_sec`.
[[nodiscard]] Result<void> write_consensus_zones(persistence::PgPool& pool,
                                                 int64_t canonical_pair_id,
                                                 std::span<const ConsensusZone> zones,
                                                 double primary_mid, double consensus_mid,
                                                 double window_confidence,
                                                 CrossVenueQuality quality,
                                                 int64_t stale_after_sec);

} // namespace alttech::crossvenue::store
