#pragma once

#include "altmacro/core/http.hpp"
#include "altmacro/core/rate_limiter.hpp"
#include "altmacro/providers/provider.hpp"

#include <string>
#include <vector>

namespace altmacro::providers {

// FRED and ALFRED, reached without an API key.
//
// The documented FRED API needs a key, but the chart download does not:
//
//   FRED    GET fred.stlouisfed.org/graph/fredgraph.csv?id=SERIES
//           -> observation_date,SERIES
//              1962-01-02,4.06
//   ALFRED  GET alfred.stlouisfed.org/graph/alfredgraph.csv?id=SERIES&vintage_date=D
//           -> observation_date,SERIES_YYYYMMDD    (the column carries the vintage)
//
// An absent observation is written as a lone ".". It is stored as MISSING with
// no value — never as zero, which for a yield series would read as a real and
// dramatic market event.
//
// FRED documents its throttle: "Up to 2 requests per second are allowed before
// being served a 429 error code", and warns that persistent violation can lead
// to a temporary block. That is the number used here.
//
// Attribution required by the terms of use is emitted at startup and recorded
// against the provider row.
class FredProvider final : public Provider {
public:
    // `vintage_capable` selects the ALFRED host and enables fetch_vintage.
    explicit FredProvider(bool vintage_capable);

    [[nodiscard]] const ProviderMetadata& metadata() const override { return meta_; }

    // FRED publishes tens of thousands of series and no useful filter for the
    // handful we want, so discovery verifies the configured series ids exist
    // rather than enumerating a catalogue.
    [[nodiscard]] Result<std::vector<InstrumentDescriptor>>
    discover_instruments(const ShouldStop& should_stop) override;

    [[nodiscard]] Result<ReferencePage>
    fetch_historical_reference(const InstrumentDescriptor& instrument, Window window,
                               const ShouldStop& should_stop) override;

    [[nodiscard]] Result<ReleasePage>
    fetch_vintage(const InstrumentDescriptor& instrument, const std::string& vintage_date,
                  Window window, const ShouldStop& should_stop) override;

    [[nodiscard]] Result<ReferencePage>
    fetch_latest(const InstrumentDescriptor& instrument, const ShouldStop& should_stop) override;

    [[nodiscard]] Result<Health> health(const ShouldStop& should_stop) override;

    void set_series(std::vector<std::string> series_ids);

    // Exposed for tests.
    struct CsvPoint {
        std::string date;                 // ISO date as published
        std::optional<double> value;      // empty when the source wrote "."
    };
    [[nodiscard]] static Result<std::vector<CsvPoint>>
    parse_csv(const std::string& body, std::string* out_value_column);
    // A conservative availability bound, never an asserted release instant.
    [[nodiscard]] static Result<int64_t> date_only_vintage_bound(const std::string& date);

private:
    ProviderMetadata meta_;
    bool vintage_capable_;
    std::string host_;
    std::vector<std::string> series_;
    core::TokenBucket bucket_;

    [[nodiscard]] Result<core::HttpResponse>
    fetch(const std::string& target, const ShouldStop& should_stop);
};

} // namespace altmacro::providers
