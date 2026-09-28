#pragma once

#include "core/error.hpp"
#include "core/persistence_gate.hpp"
#include "persistence/pg.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altspotter::persistence {

// Hard database size limit + working thresholds (fractions of the limit).
// Overridden by the `storage:` config block; the default is the 3 GiB test
// sizing so an unconfigured instance can never fill a small disk.
struct RetentionConfig {
    int64_t hard_limit_bytes = 3LL * 1024 * 1024 * 1024;
    double warning_ratio = 0.80;
    double cleanup_ratio = 0.87;
    double critical_ratio = 0.93;
    // Release margin for the escalation ladder. A level is entered when the
    // ratio crosses its threshold and only left once the ratio falls this far
    // back below it. Without the margin the database size hovers on a threshold
    // and the persistence mode flaps every few minutes, which punches
    // budget-driven holes into the raw book and trade series and makes the
    // derived 1s/1m aggregates statistically incomparable over time.
    double hysteresis_margin = 0.03;
};

struct BudgetStatus {
    int64_t db_bytes = 0;
    int64_t hard_limit_bytes = 0;
    double ratio = 0.0;
    std::string state = "NORMAL"; // NORMAL|WARNING|CLEANUP|CRITICAL|HARD_LIMIT
    PersistenceMode mode = PersistenceMode::full;
    std::optional<int64_t> wal_bytes;
};

struct DropOutcome {
    int partitions_dropped = 0;
    std::vector<std::string> dropped;   // partition names
    int64_t bytes_before = 0;
    int64_t bytes_after = 0;
};

// pg_database_size + threshold classification. The persistence mode escalates
// with the state so raw books/trades are shed before candles/context.
// `previous_state` is the state this measurement follows (empty on the first
// pass or for a one-shot report); it applies the release margin so a state is
// held until the ratio genuinely retreats rather than flapping on a threshold.
[[nodiscard]] Result<BudgetStatus> measure_budget(PgPool& pool, const RetentionConfig& cfg,
                                                  std::string_view previous_state = {});

// Pre-create upcoming partitions for every managed parent (daily + monthly).
Result<int> ensure_partitions(PgPool& pool);

// Drop whole partitions whose upper bound is older than the retention cutoff.
// Routine pass uses each policy's target_keep; when `aggressive` (storage
// pressure) it walks policies by cleanup_priority and drops down to
// minimum_keep. Protected data (recent candles etc.) is guarded by the
// per-policy minimum_keep floor. Never issues row-level DELETEs.
[[nodiscard]] Result<DropOutcome> apply_retention(PgPool& pool, const RetentionConfig& cfg,
                                                  bool aggressive, bool dry_run);

// Capture per-partition size statistics + a storage-budget snapshot row.
Result<void> record_statistics(PgPool& pool, const BudgetStatus& budget);

// Age limits for the low-volume operational tables. These are audit/telemetry
// rows, not market data: they are unpartitioned and pruned with a bounded
// DELETE (market data is only ever dropped by partition).
struct OpsRetentionConfig {
    int ingestion_runs_days = 30;
    int rejected_events_days = 30;
    int retention_runs_days = 90;
    int budget_snapshots_days = 90;
    int table_statistics_days = 90;
    int universe_events_days = 365;
    int resolved_gaps_days = 90;
    int funding_settlements_days = 365;
};

// Delete operational rows older than their configured age. Returns the total
// number of rows removed. Each table is pruned in its own transaction so one
// failure cannot roll back the rest.
Result<int64_t> prune_operational_tables(PgPool& pool, const OpsRetentionConfig& cfg);

} // namespace altspotter::persistence
