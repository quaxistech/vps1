#pragma once

#include "alttech/domain/venue.hpp"

// Small, dependency-free pieces of feature-service scheduling/query logic.
// Keeping these here makes the failure-prone edge cases directly unit-testable
// without PostgreSQL or NATS.

#include "alttech/domain/types.hpp"
#include "alttech/talib_features/feature_engine.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace alttech::featsvc {

struct ReplayStep {
    talib_features::FeatureVector vector;
    bool reset_at_gap = false;
    bool invalid_input = false;
};

struct ReplayCursor {
    int64_t last_source_open_ms = -1;
    bool previous_input_ok = false;
};

// Feed one ordered database-replay bar while enforcing the timeframe's
// continuity contract.  A source gap starts a brand-new warmup run: indicators
// must never become VALID by accumulating unrelated fragments on either side
// of missing candles.  Duplicate/reversed rows indicate a broken source query
// and fail closed instead of silently rewriting state.
inline ReplayStep process_contiguous_replay_bar(talib_features::FeatureEngine& engine,
                                                ReplayCursor& cursor, int64_t span_ms,
                                                const talib_features::FBar& bar, bool input_ok = true) {
    if (span_ms <= 0 || bar.open_time_ms < 0) {
        throw std::invalid_argument("invalid replay bar interval");
    }
    if (cursor.last_source_open_ms >= 0 &&
        bar.open_time_ms <= cursor.last_source_open_ms) {
        throw std::invalid_argument("feature replay bars are not strictly increasing");
    }

    const bool gap = cursor.last_source_open_ms >= 0 &&
                     bar.open_time_ms != cursor.last_source_open_ms + span_ms;
    if (gap || !cursor.previous_input_ok || !input_ok) engine = talib_features::FeatureEngine{};

    ReplayStep step;
    step.reset_at_gap = gap;
    step.invalid_input = !input_ok;
    if (input_ok) {
        step.vector = engine.process_bar(bar);
    } else {
        step.vector.bar_open_time_ms = bar.open_time_ms;
        step.vector.values.assign(talib_features::FeatureEngine::feature_count(), 0.0F);
        step.vector.validity.assign((talib_features::FeatureEngine::feature_count() + 7U) / 8U, 0);
        step.vector.status = talib_features::VectorStatus::incomplete_input;
    }
    cursor.last_source_open_ms = bar.open_time_ms;
    cursor.previous_input_ok = input_ok;
    return step;
}

enum class SourceRevisionState { waiting_for_storage, current, stale };
enum class SourceBarrierDecision { waiting_for_storage, current, stale };

// JetStream retains older candle revisions after PostgreSQL has already
// committed a repair.  Retrying such a message can never succeed: the writer
// will not roll the source of truth backwards.  Only a revision newer than the
// persisted row represents writer lag and should be NAKed.
[[nodiscard]] inline SourceRevisionState source_revision_state(int64_t event_revision,
                                                               int64_t persisted_revision) {
    if (event_revision > persisted_revision) return SourceRevisionState::waiting_for_storage;
    if (event_revision < persisted_revision) return SourceRevisionState::stale;
    return SourceRevisionState::current;
}

// Equal revision is not sufficient for the cross-consumer ordering barrier:
// a repaired payload may arrive before storage-writer has compared it and
// incremented the PostgreSQL revision. Equal revision with different OHLCV is
// therefore writer lag, never a stale event that may be ACKed.
[[nodiscard]] inline SourceBarrierDecision source_barrier_decision(
    int64_t event_revision, int64_t persisted_revision, bool same_payload) {
    const auto state = source_revision_state(event_revision, persisted_revision);
    if (state == SourceRevisionState::waiting_for_storage ||
        (state == SourceRevisionState::current && !same_payload)) {
        return SourceBarrierDecision::waiting_for_storage;
    }
    return state == SourceRevisionState::stale ? SourceBarrierDecision::stale
                                               : SourceBarrierDecision::current;
}

// When a cross-consumer storage barrier stops being lag and becomes divergence.
//
// `source_barrier_decision` holds a delivery back while PostgreSQL catches up
// with an event that carries a newer revision — or the same revision with a
// different payload, which the comment above it explains is writer lag rather
// than a stale event. That reasoning is correct for lag and has no exit for
// anything else. Measured in production: one 30m batch for a single instrument
// was redelivered **1,168 times in 20 minutes** against a payload PostgreSQL
// was never going to hold, because the bars it disagreed about had already
// fallen below the replay floor and would never be rewritten.
//
// An unbounded wait on a peer's state is the same failure as P0-1: the message
// occupies the consumer's ack-pending budget forever and blocks live traffic
// behind it. The wait is therefore bounded by JetStream's OWN redelivery
// counter — authoritative, survives restarts, and needs no local state. Past
// the bound the event is treated as what it is: a divergence from the
// authoritative store, to be reported and recorded rather than waited on.
//
// `max_deliveries <= 0` keeps the wait unbounded, which is the only correct
// reading of "no limit configured".
[[nodiscard]] inline bool storage_barrier_exhausted(uint64_t delivery_count,
                                                    int64_t max_deliveries) noexcept {
    if (max_deliveries <= 0) return false;
    return delivery_count > static_cast<uint64_t>(max_deliveries);
}

enum class TaskMode { live, backfill, recompute };

[[nodiscard]] inline const char* task_mode_name(TaskMode mode) {
    switch (mode) {
    case TaskMode::live: return "LIVE";
    case TaskMode::backfill: return "BACKFILL";
    case TaskMode::recompute: return "RECOMPUTE";
    }
    return "LIVE";
}

struct PendingTask {
    TaskMode mode = TaskMode::live;
    int64_t from_ms = -2; // RECOMPUTE is inclusive; LIVE=-2, BACKFILL=-1
    unsigned attempt = 0;
};

// Merge duplicate work for one shard.  A full backfill dominates every
// narrower task: first_missing_vector_time() already repairs revised rows as
// well as historical holes, while replacing a queued backfill with a live
// recompute can permanently leave old history empty.  Outside bootstrap, a
// revision dominates ordinary catch-up and retains the earliest affected bar.
// This bounds the global queue to one entry per shard even during a large
// history/reconciliation burst.
inline void merge_pending_task(PendingTask& pending, const PendingTask& incoming) {
    pending.attempt = std::max(pending.attempt, incoming.attempt);
    if (pending.mode == TaskMode::backfill || incoming.mode == TaskMode::backfill) {
        pending.mode = TaskMode::backfill;
        pending.from_ms = -1;
        return;
    }
    if (pending.mode == TaskMode::recompute && incoming.mode == TaskMode::recompute) {
        pending.from_ms = std::min(pending.from_ms, incoming.from_ms);
        return;
    }
    if (incoming.mode == TaskMode::recompute) {
        pending.mode = TaskMode::recompute;
        pending.from_ms = incoming.from_ms;
        return;
    }
    if (pending.mode == TaskMode::recompute) return;
    pending.mode = TaskMode::live;
    pending.from_ms = -2;
}

struct CandleQueryPlan {
    std::string sql;
    bool one_minute = false;
    bool open_ended = false;
};

// Build the placeholder layout separately from execution.  In particular, an
// open-ended replay must never pass INT64_MAX through PostgreSQL to_timestamp.
[[nodiscard]] inline CandleQueryPlan candle_query_plan(Venue venue, const std::string& timeframe,
                                                        bool open_ended, int limit) {
    CandleQueryPlan plan;
    plan.one_minute = timeframe == "1m";
    plan.open_ended = open_ended;
    plan.sql =
        "SELECT (extract(epoch FROM open_time) * 1000)::bigint, open::float8, high::float8, "
        "low::float8, close::float8, base_volume::float8, quote_volume::float8, revision, "
        "quality_status = 'ok' "
        "FROM ";
    plan.sql += plan.one_minute ? "market_candles_1m" : "market_candles_agg";
    plan.sql += " WHERE exchange_id = " + std::to_string(venue_id(venue)) +
                " AND instrument_id = $1 AND is_closed";
    plan.sql += " AND open_time >= to_timestamp($2::bigint / 1000.0)";
    if (!open_ended) plan.sql += " AND open_time < to_timestamp($3::bigint / 1000.0)";
    if (!plan.one_minute) {
        plan.sql += open_ended ? " AND timeframe = $3" : " AND timeframe = $4";
    }
    plan.sql += " ORDER BY open_time LIMIT " + std::to_string(limit);
    return plan;
}

// Fetch EXACTLY the named bars, never a prefix of the range that contains them.
//
// `candle_query_plan` orders by open_time and applies a LIMIT, which is right
// for a replay walking forward through a contiguous window. It is wrong for
// verifying a batch: the batch handler sized the limit by how many bars the
// BATCH held while deriving the range from their min and max, so a SPARSE batch
// asked for a window holding far more candles than the limit allowed and the
// bars at the end of it were never returned. The handler then saw a bar it had
// been sent as absent from storage and waited for a write that had already
// happened.
//
// Measured: instrument 25, 30m, a batch spanning 08-21 23:30 to 08-24 01:30 —
// fifty hours, a hundred bar slots — redelivered **8,599 times** because the
// bar at the far end of that span fell outside its own query's LIMIT.
//
// Asking for the exact timestamps cannot truncate, and it reads exactly as many
// rows as the caller intends to check.
[[nodiscard]] inline std::string candles_at_query(Venue venue, const std::string& timeframe) {
    std::string sql =
        "SELECT (extract(epoch FROM open_time) * 1000)::bigint, open::float8, high::float8, "
        "low::float8, close::float8, base_volume::float8, quote_volume::float8, revision, "
        "quality_status = 'ok' "
        "FROM ";
    const bool one_minute = timeframe == "1m";
    sql += one_minute ? "market_candles_1m" : "market_candles_agg";
    sql +=
        " WHERE exchange_id = " + std::to_string(venue_id(venue)) +
        " AND instrument_id = $1 AND is_closed"
        " AND open_time = ANY (SELECT to_timestamp(t / 1000.0) "
        "                      FROM unnest($2::bigint[]) AS t)";
    if (!one_minute) sql += " AND timeframe = $3";
    sql += " ORDER BY open_time";
    return sql;
}

// The replay horizon applies to every timeframe. It is a storage decision, not
// a per-timeframe one: analytics may only be written where a partition exists,
// and partitions only exist inside the retention window. Searching for missing
// vectors uses the same floor as the replay that follows, so an intentionally
// excluded old candle cannot stay the "first missing" row forever and force a
// redundant rewrite on every restart.
[[nodiscard]] inline int64_t bounded_replay_floor_ms(Timeframe timeframe,
                                                     int64_t backfill_days,
                                                     int64_t now_ms) noexcept {
    (void)timeframe;
    if (backfill_days <= 0 || now_ms <= 0) return 0;
    constexpr int64_t kDayMs = 86'400'000;
    if (backfill_days > now_ms / kDayMs) return 0;
    return now_ms - backfill_days * kDayMs;
}

// What part of a revised candle batch this module may act on.
//
// The replay floor bounds what can be WRITTEN. A batch entirely below it is
// nothing to do; a batch entirely above it is handled as-is. The case that
// matters is the batch that STRADDLES the floor: its oldest bars can never be
// stored, so requiring a vector for them — as the completeness check did —
// is a demand that can never be met, and the delivery is retried forever.
//
// In production that loop pinned 275 messages in permanent redelivery,
// saturated the consumer's ack-pending budget, and starved the live queue for
// 6.8 days while every service reported itself healthy. The batch is therefore
// honoured from the oldest bar that CAN be stored.
struct BatchRange {
    bool actionable = false;   // false => acknowledge, there is nothing to do
    int64_t from_ms = 0;       // oldest bar this module may store
    bool clamped = false;      // the batch straddled the floor
};

[[nodiscard]] inline BatchRange batch_processing_range(int64_t earliest_ms, int64_t latest_ms,
                                                       int64_t floor_ms) noexcept {
    BatchRange out;
    if (earliest_ms > latest_ms) return out;              // empty or malformed
    if (floor_ms <= 0) {                                  // no horizon configured
        out.actionable = true;
        out.from_ms = earliest_ms;
        return out;
    }
    if (latest_ms < floor_ms) return out;                 // entirely out of scope
    out.actionable = true;
    out.clamped = earliest_ms < floor_ms;
    out.from_ms = out.clamped ? floor_ms : earliest_ms;
    return out;
}

// Why a bar interrupts the warm-up window, and why "incomplete" must not.
//
// The indicator engine needs `kMinValidBars` of CONTIGUOUS history before a
// vector may be published as VALID. Two very different events used to be
// collapsed into one "discard the window" branch:
//
//   * a GAP - the bar is absent. Continuity is genuinely broken: every
//     windowed indicator would silently span the hole, so the accumulated
//     context is worthless and must be dropped.
//
//   * an INCOMPLETE bar - the bar EXISTS. AltSpotter aggregated it from the
//     1m candles it had and flagged it because fewer than `expected` minutes
//     contributed. Its open, close, high and low are the true extremes of the
//     window; only volume is understated.
//
// Dropping 250 bars of context for the second case made VALID unreachable on
// every timeframe >= 15m: at a 23% incomplete rate the longest unbroken run of
// `ok` bars was 77 against a 250-bar threshold. AltStrategy decides on 15m, so
// the chain below it could never assemble a DecisionContext at all.
//
// An incomplete bar therefore preserves the window; the vector emitted for
// that bar is still marked INCOMPLETE_INPUT, so quality is reported downstream
// rather than hidden.
[[nodiscard]] inline bool warmup_context_survives(bool gap, bool quality_ok) {
    (void)quality_ok;
    return !gap;
}

// What a bar publishes, given its source quality and the engine's window.
//
// An under-filled bar contributes to the series but must never be published as
// VALID, however warm the window around it is: its volume is understated, so
// the volume-weighted features computed AT it are not trustworthy. Keeping
// this a single expression is what stops a later refactor from quietly letting
// an incomplete bar through as a normal vector.
[[nodiscard]] inline std::string_view published_vector_quality(bool quality_ok,
                                                               bool engine_valid) noexcept {
    if (!quality_ok) return "INCOMPLETE_INPUT";
    return engine_valid ? "VALID" : "WARMING";
}

struct MissingVectorQueryPlan {
    std::string sql;
    bool one_minute = false;
};

// Build the bounded missing-vector query separately so the DB selection and
// replay floor remain a directly testable contract.
[[nodiscard]] inline MissingVectorQueryPlan missing_vector_query_plan(
    Venue venue, const std::string& timeframe) {
    MissingVectorQueryPlan plan;
    plan.one_minute = timeframe == "1m";
    plan.sql = "SELECT (extract(epoch FROM c.open_time) * 1000)::bigint FROM ";
    plan.sql += plan.one_minute ? "market_candles_1m c" : "market_candles_agg c";
    plan.sql +=
        " WHERE c.exchange_id = " + std::to_string(venue_id(venue)) +
        " AND c.instrument_id = $1 AND c.is_closed ";
    if (!plan.one_minute) plan.sql += "AND c.timeframe = $2 ";
    plan.sql +=
        "AND c.open_time >= to_timestamp($5::bigint / 1000.0) "
        "AND NOT EXISTS (SELECT 1 FROM alttech.venue_feature_vectors v "
        "WHERE v.venue_class = '" + std::string(venue_feature_class(venue)) + "' "
        "AND v.exchange_id = c.exchange_id "
        "AND v.canonical_pair_id = c.instrument_id "
        "AND v.timeframe = $2 AND v.bar_open_time = c.open_time "
        "AND v.bar_revision = c.revision AND v.feature_set_version_id = $3 "
        "AND v.engine_version = $4) ORDER BY c.open_time LIMIT 1";
    return plan;
}

} // namespace alttech::featsvc
