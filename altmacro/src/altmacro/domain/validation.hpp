#pragma once

#include "altmacro/core/error.hpp"
#include "altmacro/domain/series.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace altmacro::domain {

// Bounds a decoded price must land inside. These exist because the single most
// dangerous failure in this service is silent and arithmetically valid: a
// wrong decode scale yields a perfectly well-formed series that is off by a
// factor of ten, passes every ordering check, and would be discovered months
// later by a model that learned from it.
struct SanityBand {
    double min = 0.0;
    double max = 0.0;
    [[nodiscard]] bool contains(double v) const noexcept { return v >= min && v <= max; }
    [[nodiscard]] bool configured() const noexcept { return max > min; }
};

struct ValidationIssue {
    std::string rule;
    std::string detail;
    int64_t at_ms = 0;
};

struct ValidationReport {
    int64_t checked = 0;
    int64_t valid = 0;
    // Carried-forward rows, counted apart from valid ones. On this archive
    // they are roughly a third of every FX series -- weekends and quiet
    // minutes -- so folding them into `valid` would make every coverage figure
    // overstate how much was actually observed.
    int64_t stale = 0;
    int64_t market_closed = 0;
    int64_t rejected = 0;
    std::vector<ValidationIssue> issues;   // capped; see kMaxReportedIssues

    [[nodiscard]] bool clean() const noexcept { return rejected == 0; }
};

inline constexpr size_t kMaxReportedIssues = 50;

// Validate one instrument's bars as a series, not row by row: ordering and
// duplication are properties of the sequence.
//
// Nothing is repaired. A violation removes the offending bar and records why;
// silently correcting a price would turn a provider defect into our own
// fabricated observation, which is exactly the failure this service exists to
// prevent.
[[nodiscard]] ValidationReport
validate_bars(std::vector<PriceBar>& bars, const SanityBand& band);

// Validate a bid series against its ask counterpart. Called only when both
// sides were fetched for the same instrument and window; a crossed book is a
// decode or alignment error, never a market fact at this resolution.
[[nodiscard]] ValidationReport
validate_bid_ask(const std::vector<PriceBar>& bid, const std::vector<PriceBar>& ask);

[[nodiscard]] ValidationReport
validate_reference(std::vector<ReferenceValue>& values, const SanityBand& band);

} // namespace altmacro::domain
