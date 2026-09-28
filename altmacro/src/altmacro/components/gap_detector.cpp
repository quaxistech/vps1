#include "altmacro/components/gap_detector.hpp"

#include "altmacro/core/logger.hpp"
#include "altmacro/core/time.hpp"
#include "altmacro/domain/calendar.hpp"

namespace altmacro::components {
namespace {
constexpr int64_t kDayMs = 86'400'000;
constexpr int64_t kMinutesPerDay = 1440;
} // namespace

Result<std::vector<GapDetector::Finding>>
GapDetector::scan(const std::vector<data::SourceBinding>& bindings, int64_t from_ms,
                  int64_t to_ms) {
    std::vector<Finding> findings;

    auto coverage = store_.coverage_report();
    if (!coverage) return std::unexpected(coverage.error());

    for (const auto& b : bindings) {
        if (b.series_class != "MARKET") {
            // A reference series is complete when it holds every date the
            // source served. The bootstrap upserts the source's whole reply, so
            // the only interesting question is how much of it the source itself
            // marked absent — recorded as MISSING rows, not as gaps.
            continue;
        }

        // Per-day row counts. A day is complete at 1440 records, whatever their
        // quality: the MARKET_CLOSED markers are part of the day, not holes in
        // it, and counting only VALID rows would report every weekend as a
        // defect.
        auto per_day = store_.per_day_bar_counts(b.instrument_id, b.provider_id, "1m",
                                                 b.price_basis, from_ms, to_ms);
        if (!per_day) return std::unexpected(per_day.error());

        // How far this series was actually walked. Everything at or beyond the
        // watermark was never requested, and a day nobody asked about cannot be
        // called closed, missing or anything else.
        auto cursor = store_.get_cursor(b.instrument_id, b.provider_id, "1m", b.price_basis);
        if (!cursor) return std::unexpected(cursor.error());
        const int64_t walked_to = cursor->has_value() ? (*cursor)->watermark_ms : from_ms;

        for (int64_t day = (from_ms / kDayMs) * kDayMs; day < to_ms; day += kDayMs) {
            const auto it = per_day->find(day);
            const int64_t present = it == per_day->end() ? 0 : it->second;
            if (present >= kMinutesPerDay) continue;

            if (day >= walked_to) {
                // Beyond the walk. Reported so the shortfall is visible, but
                // deliberately NOT recorded as a gap: a gap is a hole inside
                // ground that was covered, and this is simply ground that was
                // not.
                Finding unwalked;
                unwalked.instrument_code = b.instrument_code;
                unwalked.provider_code = b.provider_code;
                unwalked.timeframe = "1m";
                unwalked.price_basis = domain::to_string(b.price_basis);
                unwalked.start_ms = day;
                unwalked.end_ms = day + kDayMs;
                unwalked.rows_present = present;
                unwalked.rows_expected = kMinutesPerDay;
                unwalked.classification = "not_walked";
                findings.push_back(unwalked);
                continue;
            }

            Finding f;
            f.instrument_code = b.instrument_code;
            f.provider_code = b.provider_code;
            f.timeframe = "1m";
            f.price_basis = domain::to_string(b.price_basis);
            f.start_ms = day;
            f.end_ms = day + kDayMs;
            f.rows_present = present;
            f.rows_expected = kMinutesPerDay;

            const auto cal = domain::calendar_from_string(b.session_calendar);
            // Midday is used to classify the day, because the week's boundaries
            // fall at its edges and the middle is unambiguous.
            const auto expectation = domain::expectation_at(cal, day + kDayMs / 2);

            if (present == 0) {
                // The whole day is absent. On a weekend that is the archive
                // saying the market was shut, which it genuinely was.
                f.classification = (expectation == domain::Expectation::closed)
                                       ? "market_closed"
                                       : "data_missing";
            } else {
                f.classification = "partial_day";
            }
            findings.push_back(f);

            auto rec = store_.record_gap(b.instrument_id, b.provider_id, "1m", f.start_ms,
                                         f.end_ms, static_cast<int>(kMinutesPerDay),
                                         f.classification == "market_closed" ? "market_closed"
                                                                             : "unresolved");
            if (!rec) return std::unexpected(rec.error());
        }
    }

    int64_t missing = 0, closed = 0, partial = 0, unwalked = 0;
    for (const auto& f : findings) {
        if (f.classification == "data_missing") ++missing;
        else if (f.classification == "market_closed") ++closed;
        else if (f.classification == "not_walked") ++unwalked;
        else ++partial;
    }
    log::info("gap_scan_complete",
              {{"findings", findings.size()}, {"data_missing", missing},
               {"market_closed", closed}, {"partial_day", partial},
               // Not a defect and not a gap: days the walk has not reached.
               // Kept separate so a shortfall in coverage never reads as a
               // finding about the market.
               {"not_walked", unwalked}});
    return findings;
}

} // namespace altmacro::components
