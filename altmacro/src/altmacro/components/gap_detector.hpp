#pragma once

#include "altmacro/core/error.hpp"
#include "altmacro/data/store.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace altmacro::components {

// Finds stretches where observations were expected and did not arrive.
//
// The definition of "expected" is taken from the source, not from a holiday
// table we do not have:
//
//   * Dukascopy publishes one file per calendar day containing exactly 1440
//     minute records, and writes a minute with no quotes as all-zero. So a
//     complete day is 1440 stored rows — some VALID, some MARKET_CLOSED — and
//     anything less is a day we did not finish fetching.
//   * FRED and Frankfurter emit one row per date they consider valid. Their
//     row set IS the calendar, so a hole can only mean we dropped something.
//
// This is why the detector can be strict without inventing expectations: it
// compares what we stored against what the source itself delivered.
//
// And it only judges ground the walk actually covered. A day beyond a series'
// watermark was never requested, so nothing is known about it -- reading a
// weekend there as `market_closed` would assert the market was shut on a day
// nobody asked about, which is a claim about the world made from a calendar
// rather than from evidence.
class GapDetector {
public:
    explicit GapDetector(data::Store& store) : store_(store) {}

    struct Finding {
        std::string instrument_code;
        std::string provider_code;
        std::string timeframe;
        std::string price_basis;
        int64_t start_ms = 0;
        int64_t end_ms = 0;
        int64_t rows_present = 0;
        int64_t rows_expected = 0;
        // market_closed | data_missing | provider_delay | partial_day | not_walked
        std::string classification;
    };

    [[nodiscard]] Result<std::vector<Finding>>
    scan(const std::vector<data::SourceBinding>& bindings, int64_t from_ms, int64_t to_ms);

private:
    data::Store& store_;
};

} // namespace altmacro::components
