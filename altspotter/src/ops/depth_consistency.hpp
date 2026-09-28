#pragma once

// Depth-vs-retention consistency, extracted so it can be tested without a
// database or a config file.
//
// Every rule here exists because its violation caused a real, measured
// incident on VPS1: data was fetched from a venue, written, and then deleted
// by the component whose retention was never updated alongside the depth
// setting. The failures were silent -- the backfill reported success while
// retention removed its output -- so they are validated at start-up rather
// than left to inspection.

#include <cstdint>
#include <string>
#include <vector>

namespace altspotter::ops {

// The knobs that must agree. Units are as configured, not normalised.
struct DepthSettings {
    int64_t bootstrap_days = 0;          // market_history.bootstrap_days
    int64_t candles_1m_days = 0;         // retention.candles_1m_days
    int64_t mark_index_backfill_days = 0; // <venue>_runtime.mark_index_backfill_days
    int64_t derivatives_context_days = 0; // retention.derivatives_context_days
    int64_t funding_backfill_days = 0;   // <venue>_runtime.funding_backfill_days
    int64_t funding_settlements_days = 0; // ops_retention.funding_settlements_days
    int64_t candles_agg_days = 0;        // retention.candles_agg_days

    // Storage gate.
    int64_t hard_database_limit_bytes = 0;
    double cleanup_ratio = 0.87;
    int64_t raw_trades_core_hours = 0;
    int64_t raw_trades_other_hours = 0;
};

// Bytes the configured retention is expected to hold. Constants measured on
// VPS1 with 27 instruments x 4 venues:
//   1m candles      155 520 rows/day  @ 225 B
//   aggregates       50 004 rows/day  @ 274 B  (5m+15m+30m+1h+4h+1d)
//   raw trades core ~692 000 rows/h   @ 270 B
//   raw trades other ~178 000 rows/h  @ 270 B
// Everything not modelled (1s aggregates, books, 1m series, derivatives,
// indexes on small tables) measured at ~4.6 GB.
[[nodiscard]] inline double projected_bytes(const DepthSettings& s) {
    return static_cast<double>(s.candles_1m_days) * 155'520 * 225 +
           static_cast<double>(s.candles_agg_days) * 50'004 * 274 +
           static_cast<double>(s.raw_trades_core_hours) * 692'000 * 270 +
           static_cast<double>(s.raw_trades_other_hours) * 178'000 * 270 +
           4.6e9;
}

// Returns one message per violated rule; empty means consistent.
[[nodiscard]] inline std::vector<std::string> depth_violations(const DepthSettings& s) {
    std::vector<std::string> out;
    const auto n = [](int64_t v) { return std::to_string(v); };

    if (s.bootstrap_days > s.candles_1m_days) {
        out.push_back("bootstrap_days (" + n(s.bootstrap_days) +
                      ") > candles_1m_days (" + n(s.candles_1m_days) +
                      "): the backfill loads candles retention then drops");
    }
    if (s.mark_index_backfill_days > s.derivatives_context_days) {
        out.push_back("mark_index_backfill_days (" + n(s.mark_index_backfill_days) +
                      ") > derivatives_context_days (" + n(s.derivatives_context_days) +
                      "): the same contradiction in the derivatives table");
    }
    if (s.funding_backfill_days > s.funding_settlements_days) {
        out.push_back("funding_backfill_days (" + n(s.funding_backfill_days) +
                      ") > funding_settlements_days (" + n(s.funding_settlements_days) +
                      "): retention truncates the settlements just loaded");
    }
    if (s.hard_database_limit_bytes > 0) {
        const double sheds_at =
            static_cast<double>(s.hard_database_limit_bytes) * s.cleanup_ratio;
        const double projected = projected_bytes(s);
        if (projected >= sheds_at) {
            out.push_back("hard_database_limit_bytes too small: retention projects to ~" +
                          n(static_cast<int64_t>(projected / 1e9)) +
                          " GB and aggressive cleanup begins at " +
                          n(static_cast<int64_t>(sheds_at / 1e9)) + " GB");
        }
    }
    return out;
}

}  // namespace altspotter::ops
