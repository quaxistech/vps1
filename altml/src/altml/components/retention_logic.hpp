#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace altml::storage {

// Whether the storage guard is in a state it cannot get itself out of.
//
// The guard has two levers: drop partitions past `target_keep`, and — under
// hard-limit pressure — drop them down to `minimum_keep`. Both are age-based.
// If every partition the database holds is YOUNGER than `minimum_keep`, both
// levers are no-ops, and no number of retries changes that. The budget and the
// retention policy are then simply contradictory: the policy promises to keep
// more history than the budget has room for.
//
// Observed on this deployment: `pg_hard_limit_gb = 3` against a policy of
// `minimum_keep = 120 days` on `feature_vectors`, whose two existing months
// already occupy 5.2 GB. The guard logged HARD_LIMIT every five minutes for
// days, retention recorded a successful run with `partitions_dropped = 0` and
// `db_bytes_before == db_bytes_after` every single time, and the trainer
// refused every cycle for want of memory. Three components, all reporting
// accurately, describing a deadlock none of them named.

struct PolicyPressure {
    std::string table_name;
    int64_t minimum_keep_days = 0;
    // Age, in days, of the oldest partition that actually HOLDS DATA. Negative
    // when the table holds no populated partition at all.
    //
    // It must be the oldest populated one, not simply the oldest one. Measured
    // on this deployment, `feature_vectors` carries empty 24 kB partitions for
    // 202604 (145 days), 202605 (115) and 202606 (84) either side of the two
    // that hold everything — 202607 at 54 days with 1398 MB and 202608 at 23
    // days with 3775 MB. Reading the oldest BOUND said "145 days of reclaimable
    // history", which is why the guard kept retiring empty partitions and
    // freeing nothing, and why the deadlock check stayed silent. Reading the
    // oldest POPULATED bound says 54 days, which is the truth: everything this
    // table holds is inside its own 120-day floor.
    int64_t oldest_partition_age_days = -1;
};

// True when the table's whole history is protected by its own retention floor,
// so nothing can be reclaimed from it however hard the guard tries.
[[nodiscard]] inline bool floor_blocks_reclaim(const PolicyPressure& p) noexcept {
    if (p.oldest_partition_age_days < 0) return false;   // no populated partition
    return p.oldest_partition_age_days < p.minimum_keep_days;
}

// True when the guard is over its hard limit, has just freed nothing, and no
// policy can yield anything. This is a configuration contradiction, not a
// transient shortfall, and it must be reported as one.
//
// Progress is measured in BYTES, not in partitions dropped. Measured on this
// deployment, the cycles reporting `partitions_dropped = 2` freed
// **−131,072 bytes** — the database grew: two empty partitions were retired
// while the live ones kept filling. A partition count is a record of activity,
// not of reclaim, and using it here would have silenced the report on exactly
// the cycles that looked busiest.
//
// A policy holding no partitions at all — `label_computation_runs` is a plain
// table, not a partitioned one — has nothing to contribute either way. It must
// be SKIPPED rather than counted as evidence against a deadlock: treating it as
// "not blocked" let one unpartitioned policy veto the verdict for the two
// tables that actually held the 5.2 GB, and the deadlock went unreported on a
// system that was plainly in one.
[[nodiscard]] inline bool is_retention_deadlock(bool at_hard_limit, int64_t bytes_freed,
                                                const std::vector<PolicyPressure>& policies) noexcept {
    if (!at_hard_limit || bytes_freed > 0) return false;
    bool any_blocked = false;
    for (const auto& p : policies) {
        if (p.oldest_partition_age_days < 0) continue;   // holds nothing; abstains
        if (!floor_blocks_reclaim(p)) return false;      // this one can still yield
        any_blocked = true;
    }
    return any_blocked;
}

} // namespace altml::storage
