#pragma once

#include <cstdint>

namespace altspotter::history {

// Who owns an ingestion_runs row that says 'running'.
//
// market_history inserts a bootstrap or repair run as 'running' before it
// fetches anything and records the result when it is done. A process that dies
// in between -- SIGKILL, OOM, power loss -- leaves the row 'running' for good:
// nothing else ever closes it, and retention deletes it only after
// ingestion_runs_days (7 on VPS1). A reader that takes 'running' to mean "rows
// may still be arriving" then acts on a run that ended long ago; bar_aggregation
// rescanned such a run's range on every periodic pass.
//
// Age cannot tell a stale row from a live one. A bootstrap legitimately runs for
// as long as the venue's paging and rate limits make it, and a run carries no
// heartbeat, so any age threshold either closes live runs or keeps dead ones.
// Ownership can, and it is defined by the database's own session lifecycle:
//
//   * The owner is the SESSION holding a session-level advisory lock -- not a
//     process. market_history opens a connection of its own for it. PostgreSQL
//     releases the lock when that session ends, however it ends: process death,
//     power loss, a terminated backend, a PostgreSQL restart.
//   * Run state is written ONLY on the owner session (history_owner.hpp). A
//     session that has ended cannot commit anything, so a process that lost its
//     session is fenced: it cannot start, finish or overwrite a run, even while
//     the process itself is still alive. There is never a second writer.
//   * A process whose owner session is gone stops being the owner at once. It
//     notices within one tick of its main loop (or at its next run-state write,
//     which fails), stops its bootstrap, repair and reconcile work, and ends the
//     component; the supervisor starts a fresh incarnation, which competes for
//     the lock like any other.
//   * An incarnation that does not hold the lock does nothing but wait for it
//     (standby). It creates no run, fetches no repair command, calls no venue.
//
// So when an incarnation takes the lock, every 'running' row was written by a
// session that no longer exists: either its process is dead, or its process is
// alive but fenced and stopping. Neither can ever record that run's result, so
// the new owner closes each one exactly as a clean shutdown would have:
// 'aborted', and a bootstrap's coverage back to 'pending' so it is resumed on
// the next scan. A repair needs no restart of its own: its command is a
// JetStream message the old owner never acknowledged, and the history-repair
// consumer redelivers it (AckWait 60 s, MaxDeliver unlimited; a fenced owner
// NAKs it at once).
//
// A live process with a lost session is therefore not told apart from a dead
// one, and does not need to be: it can no longer act on its runs, and it stops.

// "alts" -- the advisory-lock namespace `altspotter migrate` uses -- with a key
// of its own. Two int4 keys never collide with migrate's single bigint key.
inline constexpr int32_t kRunOwnerLockSpace = 0x616C7473;
inline constexpr int32_t kRunOwnerLockKey = 1;

// Tried by an incarnation until it succeeds, on its dedicated connection.
inline constexpr const char* kTakeRunOwnershipSql =
    "SELECT pg_try_advisory_lock($1::int4, $2::int4)";

// Whether the session running this statement holds the lock -- the owner's
// check on itself. It fails outright when the session is gone.
inline constexpr const char* kHoldsRunOwnershipSql =
    "SELECT EXISTS (SELECT 1 FROM pg_locks "
    "  WHERE locktype = 'advisory' AND granted AND objsubid = 2 AND pid = pg_backend_pid() "
    "    AND classid = $1::int4::oid AND objid = $2::int4::oid)";

// Whether any session holds the lock. Advisory locks are per database, and a
// test cluster holds many databases, so the database is part of the identity.
inline constexpr const char* kRunOwnerAliveSql =
    "SELECT EXISTS (SELECT 1 FROM pg_locks "
    "  WHERE locktype = 'advisory' AND granted AND objsubid = 2 "
    "    AND database = (SELECT oid FROM pg_database WHERE datname = current_database()) "
    "    AND classid = $1::int4::oid AND objid = $2::int4::oid)";

// How market_history records a run's result. Only a run that is still
// 'running' is finished: one a newer owner already closed keeps that owner's
// verdict, and the zero-row result tells the old incarnation it was fenced.
//   bootstrap: $1 id  $2 status  $3 error  $4 actual_from ms  $5 actual_to ms
//              $6 rows_received  $7 rows_published  $8 request_count  $9 error_count
inline constexpr const char* kFinishBootstrapRunSql =
    "UPDATE ingestion_runs SET status = $2, error = NULLIF($3, ''), "
    "actual_from = CASE WHEN $4::bigint > 0 THEN to_timestamp($4::bigint / 1000.0) END, "
    "actual_to = CASE WHEN $5::bigint > 0 THEN to_timestamp($5::bigint / 1000.0) END, "
    "rows_received = $6, rows_published = $7, request_count = $8, "
    "error_count = $9, finished_at = now() WHERE id = $1 AND status = 'running'";
//   repair:    $1 id  $2 status  $3 error  $4 rows_received  $5 rows_published
inline constexpr const char* kFinishRepairRunSql =
    "UPDATE ingestion_runs SET status = $2, error = NULLIF($3, ''), "
    "rows_received = $4, rows_published = $5, finished_at = now() "
    "WHERE id = $1 AND status = 'running'";

// Close every orphaned run and hand orphaned bootstraps back to the scheduler.
// Only ever executed while holding the lock. Returns one row per closed run:
// id, kind, exchange_id, instrument_id, requested_from, requested_to (ms),
// started_at (ms), and whether its bootstrap coverage was set back to pending.
inline constexpr const char* kCloseOrphanedRunsSql =
    "WITH orphaned AS ("
    "  UPDATE ingestion_runs SET status = 'aborted', finished_at = now(), "
    "         error = 'orphaned: the market_history owner session that ran it "
    "ended without recording a result' "
    "  WHERE status = 'running' "
    "  RETURNING id, kind, exchange_id, instrument_id, timeframe, "
    "            requested_from, requested_to, started_at), "
    "resumed AS ("
    "  UPDATE history_coverage hc SET status = 'pending', updated_at = now() "
    "  FROM orphaned o "
    "  WHERE o.kind = 'bootstrap' AND hc.exchange_id = o.exchange_id "
    "    AND hc.instrument_id = o.instrument_id AND hc.timeframe = o.timeframe "
    "    AND hc.status = 'bootstrapping' "
    "  RETURNING hc.exchange_id, hc.instrument_id) "
    "SELECT o.id, o.kind, o.exchange_id, o.instrument_id, "
    "       (extract(epoch FROM o.requested_from) * 1000)::bigint, "
    "       (extract(epoch FROM o.requested_to) * 1000)::bigint, "
    "       (extract(epoch FROM o.started_at) * 1000)::bigint, "
    "       o.kind = 'bootstrap' AND EXISTS (SELECT 1 FROM resumed r "
    "         WHERE r.exchange_id = o.exchange_id AND r.instrument_id = o.instrument_id) "
    "FROM orphaned o ORDER BY o.id";

} // namespace altspotter::history
