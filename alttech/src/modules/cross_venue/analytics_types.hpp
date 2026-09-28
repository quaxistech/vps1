#pragma once
// Pure cross-venue analytics domain types.
//
// These carry the per-venue inputs and the aggregated outputs of the
// deterministic cross-venue engine. They have no PostgreSQL, NATS or protobuf
// dependency so the estimators are unit-testable in isolation.

#include "alttech/domain/venue.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace alttech::crossvenue {

// Live per-venue data-quality state (mirrors AltSpotter exchange_data_quality
// and the wire QualityState).
enum class VenueQuality : int32_t {
    valid = 1,
    degraded = 2,
    stale = 3,
    desynced = 4,
    not_available = 5,
};

[[nodiscard]] constexpr std::string_view quality_label(VenueQuality q) noexcept {
    switch (q) {
        case VenueQuality::valid: return "VALID";
        case VenueQuality::degraded: return "DEGRADED";
        case VenueQuality::stale: return "STALE";
        case VenueQuality::desynced: return "DESYNCED";
        case VenueQuality::not_available: return "NOT_AVAILABLE";
    }
    return "NOT_AVAILABLE";
}

// A venue is "valid enough" to enter consensus only when VALID or DEGRADED.
[[nodiscard]] constexpr bool usable(VenueQuality q) noexcept {
    return q == VenueQuality::valid || q == VenueQuality::degraded;
}

enum class CrossVenueQuality : int32_t {
    strict_valid = 1,
    degraded_3_of_4 = 2,
    degraded_2_of_4 = 3,
    invalid = 4,
};

[[nodiscard]] constexpr std::string_view quality_label(CrossVenueQuality q) noexcept {
    switch (q) {
        case CrossVenueQuality::strict_valid: return "STRICT_VALID";
        case CrossVenueQuality::degraded_3_of_4: return "DEGRADED_3_OF_4";
        case CrossVenueQuality::degraded_2_of_4: return "DEGRADED_2_OF_4";
        case CrossVenueQuality::invalid: return "INVALID";
    }
    return "INVALID";
}

enum class AlignmentStatus : int32_t {
    waiting = 1,
    strict_aligned = 2,
    degraded_aligned = 3,
    late_revision = 4,
    invalid = 5,
};

[[nodiscard]] constexpr std::string_view alignment_label(AlignmentStatus s) noexcept {
    switch (s) {
        case AlignmentStatus::waiting: return "WAITING";
        case AlignmentStatus::strict_aligned: return "STRICT_ALIGNED";
        case AlignmentStatus::degraded_aligned: return "DEGRADED_ALIGNED";
        case AlignmentStatus::late_revision: return "LATE_REVISION";
        case AlignmentStatus::invalid: return "INVALID";
    }
    return "INVALID";
}

enum class ConsensusMethod : int32_t {
    quality_weighted_median = 1,
    quality_weighted_mean = 2,
    trimmed_weighted_mean = 3,
};

// One venue's aligned snapshot for a single canonical bar window. Absent
// optionals are NOT_AVAILABLE and must never be read as a zero.
struct VenueSample {
    Venue venue = Venue::unknown;
    bool present = false;             // venue delivered a bar for this window
    VenueQuality quality = VenueQuality::not_available;

    // Alignment metadata.
    // How far behind this venue's own market data was, as measured by
    // AltSpotter. It is a live health signal about the venue, not the age of
    // the bar being computed: analytics run over closed bars read from durable
    // storage, so the bar's own age says nothing about the venue.
    double data_age_ms = 0.0;
    double clock_offset_ms = 0.0;     // exchange_time - local_time (EWMA)
    bool late = false;                // arrived after the watermark
    bool revised = false;             // superseded a prior revision
    int64_t bar_revision = 0;

    // Price context.
    std::optional<double> mid_price;
    std::optional<double> mark_price;
    std::optional<double> index_price;
    std::optional<double> last_price;
    std::optional<double> log_return;

    // Momentum / trend.
    std::optional<double> momentum;   // volatility-normalized
    std::optional<double> trend_score;

    // Trade flow. Volumes are USDT-normalized already.
    std::optional<double> signed_volume_usdt;
    std::optional<double> cvd_usdt;
    std::optional<double> trade_intensity;

    // Funding.
    std::optional<double> funding_rate;      // rate for its interval
    std::optional<double> funding_interval_hours;

    // Open interest.
    std::optional<double> oi_usd;   // open interest, quote units (USDT)
    std::optional<double> oi_base;  // open interest, base units -- the only one some venues report
    std::optional<double> oi_change_ratio;

    // Liquidity.
    std::optional<double> relative_spread;   // (ask-bid)/mid
    std::optional<double> depth_usdt;        // top-N USDT notional depth

    // Per-venue TECHNICAL state, from the analytics each venue already has
    // computed for it: the projection for the primary venue, the reference_lite
    // vectors for the others. The consensus previously agreed only on price,
    // flow and funding — it could not tell whether the four venues also agreed
    // on the technical picture, which is what these carry.
    std::optional<double> rsi_14;
    std::optional<double> atr_14;
    std::optional<double> realized_vol_20;
    std::optional<double> trend_alignment;   // +1 bullish stacked EMAs, -1 bearish, 0 mixed

    // Reliability priors.
    double historical_reliability = 1.0;     // [0,1], default fully reliable
    bool payload_complete = true;
};

// A per-venue contribution to one consensus estimate, retained for
// reproducibility.
struct Contribution {
    Venue venue = Venue::unknown;
    double weight = 0.0;
    double value = 0.0;
    bool included = false;
    double freshness_ms = 0.0;
    double relative_spread = 0.0;
    double depth_usdt = 0.0;
    VenueQuality quality = VenueQuality::not_available;
    std::string exclude_reason;
};

// Result of a weighted consensus over the participating venues.
struct ConsensusResult {
    bool available = false;
    double value = 0.0;
    double confidence = 0.0;        // [0,1]
    std::vector<Contribution> contributions;
};

// The aggregate quality decision for one canonical bar window.
struct QualityDecision {
    CrossVenueQuality status = CrossVenueQuality::invalid;
    uint32_t venue_mask = 0;
    uint32_t missing_venue_mask = 0;
    uint32_t stale_venue_mask = 0;
    uint32_t desynced_venue_mask = 0;
    double confidence = 0.0;
    std::vector<std::string> reasons;
};

// Lead-lag output over the four venues.
struct LeadLagResult {
    std::array<double, 4> lead_score{}; // indexed by venue_bit
    Venue dominant_leading_venue = Venue::unknown;
    double lead_confidence = 0.0;
    // Bitget lag to each reference / consensus in ms (positive: Bitget follows).
    double bitget_lag_to_binance_ms = 0.0;
    double bitget_lag_to_bybit_ms = 0.0;
    double bitget_lag_to_okx_ms = 0.0;
    double bitget_lag_to_consensus_ms = 0.0;
};

} // namespace alttech::crossvenue
