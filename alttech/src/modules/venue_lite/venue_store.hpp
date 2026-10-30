#pragma once
// PostgreSQL access for the per-venue sweeps. Reads AltSpotter market-data
// tables strictly read-only and writes only the alttech venue-scoped tables.

#include "reference_engine.hpp"

#include "alttech/domain/venue.hpp"
#include "alttech/persistence/pg.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace alttech::venuelite::store {

struct PairVenue {
    int64_t canonical_pair_id = 0;
    Venue venue = Venue::unknown;
    std::string exchange_symbol;
};

// Which per-venue output a sweep produces (selects the "already done" gate).
enum class VenueOutput { feature, regime, microstructure, structure };

// Whether a sweep covers all four venues or only the three reference venues.
// Feature and microstructure vectors for the primary venue come from the full
// engines, so their sweeps cover the reference venues only; regime and
// structure sweeps cover all four because the cross-venue consensus needs the
// primary venue on the same 1m grid.
enum class VenueScope { all_venues, references_only };

[[nodiscard]] constexpr VenueScope scope_for(VenueOutput output) noexcept {
    // The feature sweep now covers the primary venue too, but only to derive
    // the BBO block: market_bbo_1m is the one input the full TA-Lib engine
    // cannot see, because that engine works from candles. Without this the
    // venue every order is actually sent to would be the only one with no
    // intra-minute spread analytics. No indicator is computed twice -- see
    // compute_bbo_only().
    return output == VenueOutput::microstructure ? VenueScope::references_only
                                                 : VenueScope::all_venues;
}

// Eligible universe expanded to one entry per (pair, venue) within `scope`.
[[nodiscard]] Result<std::vector<PairVenue>> load_pair_venues(persistence::PgPool& pool,
                                                              VenueScope scope);

// Register (or verify) the reference feature set and the compact micro set.
[[nodiscard]] Result<int64_t> ensure_reference_feature_set(persistence::PgPool& pool);
[[nodiscard]] Result<int64_t> ensure_reference_micro_set(persistence::PgPool& pool);

// Newest closed 1m bar (ms) for this venue past the watermark that this output
// kind has not produced yet. nullopt when nothing to compute.
[[nodiscard]] Result<std::optional<int64_t>> next_venue_bar(persistence::PgPool& pool,
                                                            const PairVenue& pv, VenueOutput kind,
                                                            int64_t watermark_ms, int lookback_min,
                                                            int64_t feature_set_id);

// Bounded candle window (oldest→newest) ending at end_bar_ms for one venue.
[[nodiscard]] Result<std::vector<LiteCandle>> load_candle_window(persistence::PgPool& pool,
                                                                 const PairVenue& pv,
                                                                 int64_t end_bar_ms, int count);

// The target bar's aggregate/context row for one venue.
[[nodiscard]] Result<LiteContext> load_context(persistence::PgPool& pool, const PairVenue& pv,
                                               int64_t bar_ms);

// Persist a reference_lite venue feature vector.
[[nodiscard]] Result<void> write_venue_feature(persistence::PgPool& pool, const PairVenue& pv,
                                               int64_t bar_ms, int64_t revision,
                                               int64_t feature_set_id, const RefResult& res);

// Persist the venue regime for the bar.
[[nodiscard]] Result<void> write_venue_regime(persistence::PgPool& pool, const PairVenue& pv,
                                              int64_t bar_ms, const RefResult& res);

// Persist a compact reference_lite microstructure vector for the 1m window.
// DT-19: the committed values and validity are handed back so the same
// vector is published (they are not recomputed for the message).
[[nodiscard]] Result<void> write_venue_micro(persistence::PgPool& pool, const PairVenue& pv,
                                             int64_t bar_ms, int64_t feature_set_id,
                                             const LiteContext& ctx, double close,
                                             std::vector<float>* out_values = nullptr,
                                             std::vector<uint8_t>* out_validity = nullptr);

// Upsert the evolving venue structure zone for one (venue, pair, kind).
[[nodiscard]] Result<void> write_venue_zone(persistence::PgPool& pool, const PairVenue& pv,
                                            const SwingZone& zone);

} // namespace alttech::venuelite::store
