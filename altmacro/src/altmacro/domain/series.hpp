#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace altmacro::domain {

// What kind of number this is. The distinction is load-bearing: an offshore
// market quote, an authority's reference rate and a central-bank fixing can
// all be "USD/CNY" to three decimal places and still be three different
// series. They are never merged.
enum class SeriesClass {
    market,              // formed by a market
    official_reference,  // published by an authority as a reference
    economic_release,    // scheduled statistic, revisable
    derived,             // arithmetic performed here, recorded as such
};

// Explicit vocabulary for presence and absence. An absent number is never
// zero; a shut market is never an error.
enum class QualityStatus {
    valid,
    stale,
    missing,
    provider_error,
    market_closed,
    partial,
};

// Which side of the market a stored price is. A provider that returns mid
// where the previous one returned bid is a different series, not a fallback.
enum class PriceBasis { bid, ask, mid, last, close, fixing, not_applicable };

// What a row's timestamp refers to.
enum class TimestampSemantics { bar_open, bar_close, point_in_time, observation_date };

[[nodiscard]] const char* to_string(SeriesClass) noexcept;
[[nodiscard]] const char* to_string(QualityStatus) noexcept;
[[nodiscard]] const char* to_string(PriceBasis) noexcept;
[[nodiscard]] const char* to_string(TimestampSemantics) noexcept;

[[nodiscard]] std::optional<SeriesClass> series_class_from_string(std::string_view) noexcept;
[[nodiscard]] std::optional<QualityStatus> quality_status_from_string(std::string_view) noexcept;
[[nodiscard]] std::optional<PriceBasis> price_basis_from_string(std::string_view) noexcept;

// One OHLC bar as the provider published it, already normalised to UTC.
//
// Prices are optional because a bar can legitimately exist with no price: a
// weekend hour is recorded as MARKET_CLOSED so that its absence is a fact in
// the series rather than a hole a consumer has to guess about.
struct PriceBar {
    std::string timeframe;            // 1m, 5m, 15m, 1h, 1d
    int64_t event_time_ms = 0;        // bar OPEN, UTC
    std::optional<double> open;
    std::optional<double> high;
    std::optional<double> low;
    std::optional<double> close;
    std::optional<double> volume;     // provider's own unit; not cross-comparable
    PriceBasis basis = PriceBasis::mid;
    QualityStatus quality = QualityStatus::valid;
    std::string quality_reason;
};

// One point of a single-valued series (fixing, official rate, index level).
struct ReferenceValue {
    int64_t event_time_ms = 0;                     // what the number refers to
    std::optional<int64_t> publication_time_ms;    // when it became knowable
    std::optional<double> value;
    std::optional<int64_t> value_as_of_ms;         // set when carried forward
    QualityStatus quality = QualityStatus::valid;
    std::string quality_reason;
};

// One vintage of a revisable statistic. `observation_time` is the period the
// number describes; `publication_time` is when a model could first have seen
// it. Selecting on the former is how look-ahead gets manufactured, so both are
// always carried.
struct ReleaseObservation {
    int64_t observation_time_ms = 0;
    std::optional<int64_t> publication_time_ms;
    std::optional<std::string> vintage_date;       // ISO date of the source snapshot
    int32_t revision = 0;
    std::optional<double> value;
    QualityStatus quality = QualityStatus::valid;
    std::string quality_reason;
};

} // namespace altmacro::domain
