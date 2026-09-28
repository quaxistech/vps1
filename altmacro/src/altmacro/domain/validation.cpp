#include "altmacro/domain/validation.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace altmacro::domain {
namespace {

void note(ValidationReport& r, std::string rule, std::string detail, int64_t at) {
    if (r.issues.size() < kMaxReportedIssues) {
        r.issues.push_back(ValidationIssue{std::move(rule), std::move(detail), at});
    }
}

bool finite_positive(const std::optional<double>& v) {
    return v.has_value() && std::isfinite(*v) && *v > 0.0;
}

} // namespace

ValidationReport validate_bars(std::vector<PriceBar>& bars, const SanityBand& band) {
    ValidationReport r;
    std::vector<PriceBar> kept;
    kept.reserve(bars.size());

    int64_t previous_ms = std::numeric_limits<int64_t>::min();
    std::unordered_set<int64_t> seen;
    seen.reserve(bars.size() * 2);

    for (auto& b : bars) {
        ++r.checked;

        // Duplicates are dropped rather than merged: two rows claiming the same
        // instant cannot both be right, and choosing between them silently
        // would be a decision this layer has no basis to make.
        if (!seen.insert(b.event_time_ms).second) {
            ++r.rejected;
            note(r, "duplicate_timestamp", "a bar for this instant was already present",
                 b.event_time_ms);
            continue;
        }
        if (b.event_time_ms < previous_ms) {
            ++r.rejected;
            note(r, "out_of_order", "timestamp goes backwards within the series", b.event_time_ms);
            continue;
        }
        previous_ms = b.event_time_ms;

        if (b.quality == QualityStatus::market_closed) {
            ++r.market_closed;
            kept.push_back(std::move(b));
            continue;
        }

        if (!finite_positive(b.open) || !finite_positive(b.high) ||
            !finite_positive(b.low) || !finite_positive(b.close)) {
            ++r.rejected;
            note(r, "non_positive_price", "a price is missing, zero, negative or not finite",
                 b.event_time_ms);
            continue;
        }
        if (!(*b.high >= *b.open && *b.high >= *b.close && *b.high >= *b.low &&
              *b.low <= *b.open && *b.low <= *b.close)) {
            ++r.rejected;
            note(r, "ohlc_ordering", "high/low do not bound open and close", b.event_time_ms);
            continue;
        }
        if (b.volume.has_value() && (!std::isfinite(*b.volume) || *b.volume < 0.0)) {
            ++r.rejected;
            note(r, "negative_volume", "volume is negative or not finite", b.event_time_ms);
            continue;
        }
        if (band.configured() && !band.contains(*b.close)) {
            // This is the scale guard. It fires on a series that is internally
            // perfect and externally wrong.
            ++r.rejected;
            note(r, "outside_sanity_band",
                 "close " + std::to_string(*b.close) + " lies outside the instrument's expected "
                 "range; the decode scale is the usual cause",
                 b.event_time_ms);
            continue;
        }

        if (b.quality == QualityStatus::stale) {
            ++r.stale;
        } else {
            ++r.valid;
        }
        kept.push_back(std::move(b));
    }

    bars.swap(kept);
    return r;
}

ValidationReport validate_bid_ask(const std::vector<PriceBar>& bid,
                                  const std::vector<PriceBar>& ask) {
    ValidationReport r;
    size_t i = 0, j = 0;
    while (i < bid.size() && j < ask.size()) {
        if (bid[i].event_time_ms < ask[j].event_time_ms) { ++i; continue; }
        if (ask[j].event_time_ms < bid[i].event_time_ms) { ++j; continue; }

        const auto& b = bid[i];
        const auto& a = ask[j];
        ++r.checked;
        if (b.quality == QualityStatus::market_closed || a.quality == QualityStatus::market_closed) {
            ++r.market_closed;
        } else if (b.close.has_value() && a.close.has_value()) {
            if (*b.close > *a.close) {
                ++r.rejected;
                note(r, "crossed_book",
                     "bid close " + std::to_string(*b.close) + " exceeds ask close " +
                         std::to_string(*a.close),
                     b.event_time_ms);
            } else {
                ++r.valid;
            }
        }
        ++i; ++j;
    }
    return r;
}

ValidationReport validate_reference(std::vector<ReferenceValue>& values, const SanityBand& band) {
    ValidationReport r;
    std::vector<ReferenceValue> kept;
    kept.reserve(values.size());

    int64_t previous_ms = std::numeric_limits<int64_t>::min();
    std::unordered_set<int64_t> seen;

    for (auto& v : values) {
        ++r.checked;
        if (!seen.insert(v.event_time_ms).second) {
            ++r.rejected;
            note(r, "duplicate_timestamp", "a value for this instant was already present",
                 v.event_time_ms);
            continue;
        }
        if (v.event_time_ms < previous_ms) {
            ++r.rejected;
            note(r, "out_of_order", "observation time goes backwards", v.event_time_ms);
            continue;
        }
        previous_ms = v.event_time_ms;

        if (v.quality != QualityStatus::valid && v.quality != QualityStatus::stale) {
            // An explicit absence. It carries no value by construction, and
            // that is the point: the row records that nothing was published.
            if (v.value.has_value()) {
                ++r.rejected;
                note(r, "absent_row_carries_value",
                     "a row marked absent must not carry a number", v.event_time_ms);
                continue;
            }
            ++r.market_closed;
            kept.push_back(std::move(v));
            continue;
        }

        if (!v.value.has_value() || !std::isfinite(*v.value)) {
            ++r.rejected;
            note(r, "value_not_finite", "value is missing or not finite", v.event_time_ms);
            continue;
        }
        // Rates and yields are legitimately negative, so positivity is not
        // asserted here; the band is what constrains a reference series.
        if (band.configured() && !band.contains(*v.value)) {
            ++r.rejected;
            note(r, "outside_sanity_band",
                 "value " + std::to_string(*v.value) + " lies outside the series' expected range",
                 v.event_time_ms);
            continue;
        }

        ++r.valid;
        kept.push_back(std::move(v));
    }

    values.swap(kept);
    return r;
}

} // namespace altmacro::domain
