#pragma once
// How partition maintenance takes its locks, and why it must yield.
//
// Creating, attaching or dropping a partition needs an AccessExclusiveLock on
// the partitioned parent. Live analytics writes hold row locks on the same
// parents, and a feature task writes several of them in one transaction. With
// PostgreSQL's default `lock_timeout = 0` the maintenance run simply queues
// behind a writer, the deadlock detector fires after `deadlock_timeout`, and it
// picks whichever transaction it likes as the victim — which was measured here
// as a live feature task being cancelled by the hourly timer two seconds after
// it started.
//
// The asymmetry is the whole point: a cancelled analytics write loses a bar of
// data and has to be redone, while a partition that is not created this minute
// costs nothing as long as it exists before the data needs it. So maintenance
// must always be the side that gives way.
//
// It gives way by setting `lock_timeout` strictly *below* `deadlock_timeout`.
// PostgreSQL then cancels the waiting maintenance statement with 55P03
// (lock_not_available) before the deadlock detector ever runs, so no deadlock is
// reported and no live writer is ever chosen as the victim. The maintenance run
// backs off and tries again; writer transactions are short, so an attempt lands
// in a gap quickly.

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <pqxx/pqxx>

#include "alttech/common/error.hpp"

namespace alttech::persistence {

// Must stay below the server's `deadlock_timeout` (1s by default). At 750 ms a
// maintenance statement that is blocked aborts itself a quarter of a second
// before the detector would otherwise choose a victim.
inline constexpr int kMaintenanceLockTimeoutMs = 750;

// Attempts within one maintenance run. Four attempts with the backoff below
// span about five seconds, which is far longer than a feature transaction, so
// failing every attempt means the database is genuinely busy rather than that
// the run was unlucky.
inline constexpr int kMaintenanceLockAttempts = 4;

// Linear rather than exponential: we are waiting out a short transaction, not
// backing off from an overloaded service, and the run has a whole hour before it
// matters. Deliberately not jittered — there is only ever one maintenance run,
// serialized by its advisory lock, so there is nothing to collide with.
[[nodiscard]] inline int maintenance_backoff_ms(int attempt) {
    return attempt <= 0 ? 0 : 500 * attempt;
}

// True for the SQLSTATEs that mean "someone else holds the lock right now",
// which is a reason to try again rather than to report a failure.
[[nodiscard]] inline bool is_lock_contention(const std::string& sqlstate) {
    return sqlstate == "55P03" ||  // lock_not_available (our own lock_timeout)
           sqlstate == "40P01" ||  // deadlock_detected
           sqlstate == "40001";    // serialization_failure
}

// Applies the timeout to the current transaction. SET LOCAL so it reverts on
// commit and cannot leak into a pooled connection's later work.
inline void yield_to_writers(pqxx::transaction_base& txn) {
    txn.exec("SET LOCAL lock_timeout = '" + std::to_string(kMaintenanceLockTimeoutMs) + "ms'");
}

// Runs `body` in its own transaction with the timeout applied, retrying while
// the only thing standing in the way is another transaction's lock.
//
// `attempts_used` reports how many tries it took, so a run that is habitually
// contended shows up in the logs instead of looking instant.
template <typename Body>
Result<void> with_maintenance_lock(pqxx::connection& conn, Body&& body,
                                   int* attempts_used = nullptr) {
    std::string last;
    for (int attempt = 1; attempt <= kMaintenanceLockAttempts; ++attempt) {
        if (attempts_used != nullptr) *attempts_used = attempt;
        try {
            pqxx::work txn(conn);
            yield_to_writers(txn);
            body(txn);
            txn.commit();
            return {};
        } catch (const pqxx::sql_error& e) {
            last = e.what();
            if (!is_lock_contention(e.sqlstate())) {
                return std::unexpected(Error{ErrorCode::database_error, last, false});
            }
            if (attempt < kMaintenanceLockAttempts) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(maintenance_backoff_ms(attempt)));
            }
        } catch (const std::exception& e) {
            return std::unexpected(Error{ErrorCode::database_error, e.what(), false});
        }
    }
    // Retryable: nothing is wrong, the database was simply busy, and the next
    // run will find it idle.
    return std::unexpected(Error{ErrorCode::database_error,
                                 "partition maintenance yielded to live writers after " +
                                     std::to_string(kMaintenanceLockAttempts) +
                                     " attempts; will retry next run (" + last + ")",
                                 true});
}

} // namespace alttech::persistence
