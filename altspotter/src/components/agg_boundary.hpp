#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace altspotter::aggregation {

// Which aggregate windows may be written at all, kept pure so the defect they
// were extracted from stays covered by tests.
//
// market_candles_1m and market_candles_agg are partitioned by month and have
// separate retention horizons (candles_1m_days 280 vs candles_agg_min_days
// 200). Under disk pressure the aggregate partitions are trimmed first, so a
// month can keep its 1m partition long after its aggregate partition is gone.
// Every aggregate window in that band is, by definition, missing — and can
// never be written, because PostgreSQL rejects the insert with "no partition of
// relation found for row". The writer then falls back to its per-message
// isolation path, which is slow enough to push live candles past the downstream
// staleness budget. In production that band was 2025-12-01 .. 2026-03-01 and it
// stalled the whole pipeline from 2026-09-16 to 2026-09-22.
//
// The first fix bounded the scan by `min(open_time)` of the *rows* in
// market_candles_agg. That is not the boundary: it is a watermark over data,
// and when a key has no aggregate rows at all it collapses to the COALESCE
// fallback ('epoch'), the bound disappears and the entire 1m history becomes
// eligible again. An empty aggregate key is ambiguous — it is either a
// legitimate bootstrap (a new instrument or a newly configured timeframe) or a
// retention-degraded key (its aggregates aged out) — and a row watermark cannot
// tell those apart.
//
// The partition space can. A window is writable exactly when a partition exists
// that accepts its open_time, so the authoritative floor is the lower bound of
// the oldest surviving market_candles_agg partition. It is derived from the
// catalog (altspotter_partition_bounds), never from a date in the source, and
// it answers all three cases with one rule:
//
//   * legitimate bootstrap   -> the key's windows are inside the partition
//                               space, so they are all eligible;
//   * retention-degraded key -> its old windows are below the floor and stay
//                               excluded, whether or not any rows survive;
//   * history outside the aggregate retention -> below the floor, excluded.

// The floor has not been established yet (the catalog query has not run or
// failed). The guard must not block publication in this state: live bars are
// always inside the partition space, and refusing to publish them would be a
// worse failure than the one this guard exists to prevent.
inline constexpr int64_t kFloorUnknown = std::numeric_limits<int64_t>::min();

// market_candles_agg has no range partitions at all. Nothing can be written,
// so nothing is eligible. This is a real outage of partition maintenance, not a
// bootstrap, and callers are expected to say so in the log.
inline constexpr int64_t kNoAggregatePartition = std::numeric_limits<int64_t>::max();

[[nodiscard]] inline constexpr bool floor_is_known(int64_t floor_ms) noexcept {
    return floor_ms != kFloorUnknown;
}

// True when an aggregate row opening at `open_time_ms` has a partition to land
// in. With no floor established this is permissive by design (see above); with
// kNoAggregatePartition it is false for every window, because INT64_MAX is
// greater than any representable open_time.
[[nodiscard]] inline constexpr bool window_is_writable(int64_t open_time_ms,
                                                       int64_t floor_ms) noexcept {
    if (!floor_is_known(floor_ms)) return true;
    return open_time_ms >= floor_ms;
}

// The part of [from_ms, to_ms) that is writable, or nullopt when the whole
// range is below the floor. Used by the repair path, which is handed a range
// derived from a 1m gap and may therefore point at a month whose aggregate
// partition is gone even though its 1m partition is alive.
[[nodiscard]] inline constexpr std::optional<std::pair<int64_t, int64_t>>
clamp_to_writable(int64_t from_ms, int64_t to_ms, int64_t floor_ms) noexcept {
    if (!floor_is_known(floor_ms)) return std::pair{from_ms, to_ms};
    if (floor_ms == kNoAggregatePartition) return std::nullopt;
    const int64_t start = from_ms < floor_ms ? floor_ms : from_ms;
    if (start >= to_ms) return std::nullopt;
    return std::pair{start, to_ms};
}

// ---------------------------------------------------------------------------
// What a catch-up pass has to look at.
//
// The floor above says where aggregates CAN be written. It is not where a
// periodic pass has to look. The periodic pass exists "in case the writer lags
// behind" (bar_aggregation.cpp), yet it re-scanned every retained minute from
// the floor on every pass: on VPS1 that was ~5 s per instrument, 108
// instruments, ~540 s per pass -- and catch-up and the live cycle share one
// loop, so no bar was published for that long. Any 15m bar closing early in a
// pass reached AltStrategy more than 5 minutes after its close, past the
// 20-minute max_component_age, and every pair was declined until it arrived
// (435 pair-minutes in 6 h on 2026-09-23; 54-61% of 15m bars late before the
// poison loop ever started).
//
// A window's eligibility only changes when 1m rows for it are committed. The
// storage writer is the only writer of 1m rows, and a row older than what the
// writer had already committed (its frontier) can only arrive later by:
//   * REST reconciliation -- at most reconcile_overlap_minutes (30) back,
//     which recheck_horizon_min (35) is already sized to cover;
//   * JetStream redelivery after a NAK -- seconds (writer backoff <= 30 s);
//   * REST bootstrap or repair -- which ingestion_runs records with its range.
// So a periodic pass of a key looks at two kinds of range:
//   * the recent range: from the writer frontier observed at its previous
//     successful pass, less the recheck horizon, up to now. A writer that
//     lagged for hours leaves the frontier behind, so the range grows with the
//     lag instead of being a guess about it;
//   * the range of each REST backfill that may have committed rows since that
//     pass -- and ONLY that range.
//
// The first version folded the backfills into one lower bound, min(recent,
// requested_from), and scanned from there to now. A repair is a 1m gap, at most
// max_gap_minutes_per_command (1000) long, but it can sit anywhere in the 1m
// retention: that lower bound turned every recent repair into a scan of every
// minute between it and now. On VPS1's real history 540 of 1420 repairs lay
// wholly below the aggregate floor -- they cannot change a single writable
// window -- and still dragged the bound to the floor (5.6 s per key instead of
// 94 ms); in the 2026-09-18..21 repair wave up to 21 keys a pass did so, and
// the pass took ~100-130 s. A repair changes the windows over its own minutes,
// so that is what gets rescanned, cut to the writable region by the same rule
// as everything else (clamp_to_writable):
//   * wholly below the floor      -> nothing writable: no range at all;
//   * straddling the floor        -> [floor, end of the repair];
//   * wholly above the floor      -> the repair's own range;
//   * no backfill                 -> the recent range alone.
// A bootstrap is the same rule applied to a range that is months long: that
// depth is real (it rewrote months of 1m), and it is the one legitimately deep
// periodic scan.
//
// A key this process has not yet passed (startup, a newly listed instrument)
// has no frontier and is scanned from the floor: startup recovery and a new
// key's bootstrap keep their full reach.
// ---------------------------------------------------------------------------

// A range with no end of its own: it runs to the closed bound of the pass.
inline constexpr int64_t kOpenEnded = std::numeric_limits<int64_t>::max();

// A span of 1m open times, [from_ms, to_ms).
struct ScanRange {
    int64_t from_ms = 0;
    int64_t to_ms = kOpenEnded;
    friend constexpr bool operator==(const ScanRange&, const ScanRange&) = default;
};

// A REST backfill as ingestion_runs records it. `to_ms` is the INCLUSIVE last
// minute: market_history stores a repair command's inclusive `to_ms` and a
// bootstrap's start time there. Either end may be unknown (NULL in both the
// requested and the actual column); an unknown end is read as reaching as far
// as it could, never as nothing.
struct BackfillRun {
    std::optional<int64_t> from_ms;
    std::optional<int64_t> to_ms;
};

struct CatchUpPlan {
    std::vector<ScanRange> ranges;    // sorted, disjoint, none below the floor
    int64_t runs_below_floor = 0;     // nothing writable: skipped
    int64_t runs_clamped = 0;         // straddled the floor: cut at it
    int64_t runs_within = 0;          // wholly writable: scanned as recorded

    // Scanned from the floor to now in one piece: startup, a new key, or a
    // bootstrap that reaches the floor.
    [[nodiscard]] bool full_reach(int64_t floor_ms) const noexcept {
        return ranges.size() == 1 && ranges.front().from_ms <= floor_ms &&
               ranges.front().to_ms == kOpenEnded;
    }
};

// Sort and coalesce overlapping or touching ranges.
[[nodiscard]] inline std::vector<ScanRange> merge_ranges(std::vector<ScanRange> ranges) {
    std::sort(ranges.begin(), ranges.end(),
              [](const ScanRange& a, const ScanRange& b) { return a.from_ms < b.from_ms; });
    std::vector<ScanRange> out;
    out.reserve(ranges.size());
    for (const auto& r : ranges) {
        if (!out.empty() && r.from_ms <= out.back().to_ms) {
            if (r.to_ms > out.back().to_ms) out.back().to_ms = r.to_ms;
        } else {
            out.push_back(r);
        }
    }
    return out;
}

// The ranges one key's pass scans. `floor_ms` must be a usable floor (known,
// with at least one partition): catch-up does not run without one.
[[nodiscard]] inline CatchUpPlan plan_catchup(int64_t floor_ms,
                                             std::optional<int64_t> prev_frontier_ms,
                                             int64_t recheck_horizon_ms,
                                             const std::vector<BackfillRun>& runs) {
    CatchUpPlan plan;
    if (!prev_frontier_ms) {   // never passed: full reach, which covers every run
        plan.ranges.push_back({floor_ms, kOpenEnded});
        return plan;
    }
    std::vector<ScanRange> ranges;
    ranges.push_back({std::max(floor_ms, *prev_frontier_ms - recheck_horizon_ms), kOpenEnded});
    constexpr int64_t kMinute = 60'000;
    for (const auto& run : runs) {
        const int64_t from = run.from_ms.value_or(floor_ms);
        const int64_t to = run.to_ms && *run.to_ms < kOpenEnded - kMinute ? *run.to_ms + kMinute
                                                                          : kOpenEnded;
        const auto writable = clamp_to_writable(from, to, floor_ms);
        if (!writable) {
            ++plan.runs_below_floor;
            continue;
        }
        ++(from < floor_ms ? plan.runs_clamped : plan.runs_within);
        ranges.push_back({writable->first, writable->second});
    }
    plan.ranges = merge_ranges(std::move(ranges));
    return plan;
}

// A scan bound that does not sit on the timeframe's grid would cut the window
// it falls in, and a cut window never has all of its minutes, so it would be
// skipped. Align down (epoch origin, as date_bin does), never below the floor.
[[nodiscard]] inline constexpr int64_t scan_from_for_timeframe(int64_t lower_ms, int64_t floor_ms,
                                                               int64_t window_ms) noexcept {
    int64_t aligned = window_ms > 0 ? (lower_ms / window_ms) * window_ms : lower_ms;
    if (aligned > lower_ms) aligned -= window_ms;   // negative inputs round toward zero
    return aligned < floor_ms ? floor_ms : aligned;
}

// The plan on one timeframe's grid: each range widened to whole windows (the
// start down, the end up), bounded by the pass's exclusive `closed_bound_ms`,
// and coalesced again, since widening can make neighbours overlap. Each result
// is one execution of kFirstMissingWindowSql ($7 = from, $5 = to).
[[nodiscard]] inline std::vector<ScanRange> ranges_for_timeframe(
    const std::vector<ScanRange>& ranges, int64_t floor_ms, int64_t window_ms,
    int64_t closed_bound_ms) {
    std::vector<ScanRange> out;
    out.reserve(ranges.size());
    for (const auto& r : ranges) {
        const int64_t from = scan_from_for_timeframe(r.from_ms, floor_ms, window_ms);
        int64_t to = closed_bound_ms;
        if (r.to_ms != kOpenEnded && window_ms > 0) {
            const int64_t up = r.to_ms % window_ms == 0 ? r.to_ms
                                                        : (r.to_ms / window_ms + 1) * window_ms;
            to = std::min(up, closed_bound_ms);
        }
        if (from < to) out.push_back({from, to});
    }
    return merge_ranges(std::move(out));
}

// Minutes of 1m history a plan covers up to `now_ms`: what the pass log
// reports as a key's scan span.
[[nodiscard]] inline int64_t plan_span_ms(const std::vector<ScanRange>& ranges,
                                          int64_t now_ms) noexcept {
    int64_t span = 0;
    for (const auto& r : ranges) {
        const int64_t to = r.to_ms == kOpenEnded ? now_ms : std::min(r.to_ms, now_ms);
        if (to > r.from_ms) span += to - r.from_ms;
    }
    return span;
}

// The newest 1m minute the writer has committed for a key; NULL when none.
// A backward index-only scan of the primary key: ~0.5 ms on VPS1.
inline constexpr const char* kWriterFrontierSql =
    "SELECT (extract(epoch FROM max(open_time)) * 1000)::bigint "
    "FROM market_candles_1m WHERE exchange_id = $1 AND instrument_id = $2";

// The REST backfills (bootstrap or repair) that may have rows the writer had
// not committed at the key's previous pass: one still running, or one that
// finished after that pass's frontier less the horizon. A backfill that
// finished before the writer's frontier passed it is fully committed (one
// ordered stream, one consumer), so it is no longer looked at.
//
// 'running' counts only while a market_history incarnation is alive to be
// running it ($4, history_runs.hpp): a row that a killed process left
// 'running' is not a backfill in progress, and is not rescanned on every pass
// until retention deletes it.
//   $1 exchange_id  $2 instrument_id  $3 prev frontier - recheck horizon (ms)
//   $4 whether the run owner is alive
inline constexpr const char* kBackfillRunsSql =
    "SELECT (extract(epoch FROM COALESCE(requested_from, actual_from)) * 1000)::bigint, "
    "       (extract(epoch FROM COALESCE(requested_to, actual_to)) * 1000)::bigint "
    "FROM ingestion_runs WHERE exchange_id = $1 AND instrument_id = $2 "
    "  AND kind IN ('bootstrap', 'repair') "
    "  AND ((status = 'running' AND $4::boolean) "
    "       OR finished_at > to_timestamp($3::bigint / 1000.0))";

// The catalog read behind the floor. min(lower_bound) over the range
// partitions of market_candles_agg; NULL when there are none.
inline constexpr const char* kAggregateFloorSql =
    "SELECT (extract(epoch FROM min(lower_bound)) * 1000)::bigint "
    "FROM altspotter_partition_bounds('market_candles_agg'::regclass)";

// The catch-up scan. Kept here so the tests exercise the statement the service
// actually issues instead of a copy that can drift away from it.
//
//   $1 exchange_id  $2 instrument_id  $3 timeframe interval  $4 timeframe label
//   $5 exclusive upper bound (ms): the pass's closed bound, or the end of a
//      backfill range (ranges_for_timeframe)
//   $6 minutes per window
//   $7 scan-from (ms): the start of the range -- never below the floor,
//      aligned to the window grid.
// The aggregate side carries the same bounds. They cannot change the result
// (the EXCEPT only removes windows the left side produced, all in [$7, $5)),
// but without the lower one every pass read every aggregate row of the key,
// and without the upper one a backfill range months back read every aggregate
// row between it and now.
inline constexpr const char* kFirstMissingWindowSql =
    "SELECT (extract(epoch FROM min(win)) * 1000)::bigint FROM ("
    "  SELECT date_bin($3::interval, open_time, 'epoch'::timestamptz) AS win "
    "  FROM market_candles_1m "
    "  WHERE exchange_id = $1 AND instrument_id = $2 AND is_closed "
    "    AND open_time < to_timestamp($5::bigint / 1000.0) "
    "    AND open_time >= to_timestamp($7::bigint / 1000.0) "
    "  GROUP BY 1 HAVING count(*) = $6 "
    "  EXCEPT "
    "  SELECT open_time FROM market_candles_agg "
    "  WHERE exchange_id = $1 AND instrument_id = $2 AND timeframe = $4 "
    "    AND quality_status = 'ok' AND minute_count = $6 "
    "    AND open_time >= to_timestamp($7::bigint / 1000.0) "
    "    AND open_time < to_timestamp($5::bigint / 1000.0)"
    ") missing WHERE win < to_timestamp($5::bigint / 1000.0)";

} // namespace altspotter::aggregation
