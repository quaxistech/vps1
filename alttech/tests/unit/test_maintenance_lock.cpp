// Partition maintenance must lose every race against a live analytics write.
// The whole mechanism rests on one numeric relationship — the lock timeout
// being below the server's deadlock timeout — so that relationship is asserted
// rather than left in a comment.

#include "alttech/persistence/maintenance_lock.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace alttech::persistence;

// PostgreSQL's default, and what this deployment runs (`SHOW deadlock_timeout`).
constexpr int kServerDeadlockTimeoutMs = 1000;

TEST_CASE("maintenance aborts before the deadlock detector can pick a victim") {
    // If this ever stops holding, the detector runs first and may cancel the
    // live feature task instead of the maintenance statement — which is the
    // failure this design exists to prevent.
    CHECK(kMaintenanceLockTimeoutMs < kServerDeadlockTimeoutMs);
    // ...and not so short that it gives up during an ordinary lock handoff.
    CHECK(kMaintenanceLockTimeoutMs >= 250);
}

TEST_CASE("a run waits far longer than a writer transaction before giving up") {
    int total = 0;
    for (int attempt = 1; attempt < kMaintenanceLockAttempts; ++attempt) {
        total += maintenance_backoff_ms(attempt) + kMaintenanceLockTimeoutMs;
    }
    // Feature transactions are milliseconds; a run that fails every attempt
    // across several seconds means the database is busy, not that it was
    // unlucky.
    CHECK(total > 3000);
}

TEST_CASE("backoff grows and is defined at the edges") {
    CHECK(maintenance_backoff_ms(0) == 0);
    CHECK(maintenance_backoff_ms(-1) == 0);
    CHECK(maintenance_backoff_ms(1) < maintenance_backoff_ms(2));
    CHECK(maintenance_backoff_ms(2) < maintenance_backoff_ms(3));
}

TEST_CASE("only lock contention is retried") {
    CHECK(is_lock_contention("55P03")); // our own lock_timeout fired
    CHECK(is_lock_contention("40P01")); // deadlock detected
    CHECK(is_lock_contention("40001")); // serialization failure

    // These mean the schema or the grant is wrong. Retrying cannot fix them,
    // and treating them as contention would hide a real fault behind a
    // "deferred to the next run" message.
    CHECK_FALSE(is_lock_contention("42P01")); // undefined_table
    CHECK_FALSE(is_lock_contention("3F000")); // invalid_schema_name
    CHECK_FALSE(is_lock_contention("42501")); // insufficient_privilege
    CHECK_FALSE(is_lock_contention("23505")); // unique_violation
    CHECK_FALSE(is_lock_contention(""));
}
