// alttech-analytics: operational tooling for the AltTech-owned analytics schema.
//
//   alttech-analytics storage-inspect [--record]
//   alttech-analytics retention-run [--apply]
//   alttech-analytics cross-venue-inspect [--pair N]
//   alttech-analytics alignment-inspect  [--pair N]
//
// Every subcommand writes or drops only inside the alttech schema; the public
// (AltSpotter) schema is measured read-only and never modified.
//
// The storage budget is read from the service configuration, never hardcoded:
//   --config FILE (default /etc/alttech/alttech.yaml)
// DSN precedence: --dsn, ALTTECH_POSTGRES_DSN.
//
// retention-run works in two stages. First it drops every partition older than
// its policy's target_keep. If the schema is still at or above the configured
// cleanup ratio of its allocation it enters budget pressure and keeps dropping
// the oldest partitions in cleanup_priority order, never cutting a family below
// its minimum_keep.

#include "alttech/persistence/maintenance_lock.hpp"
#include "alttech/config/analytics_config.hpp"
#include "alttech/config/config.hpp"

#include <pqxx/pqxx>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int64_t kRetentionLockKey = 0x616C7472; // 'altr'

struct Budget {
    int64_t hard_limit = 20'000'000'000;
    int64_t soft_limit = 3'000'000'000;
    double warning_ratio = 0.80;
    double cleanup_ratio = 0.90;
    double critical_ratio = 0.95;
};

std::string human_bytes(int64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
    return buf;
}

const char* budget_state(const Budget& budget, int64_t analytics_bytes, int64_t db_bytes) {
    if (db_bytes >= budget.hard_limit) return "HARD_LIMIT";
    const double ratio =
        static_cast<double>(analytics_bytes) / static_cast<double>(budget.soft_limit);
    if (ratio >= budget.critical_ratio) return "CRITICAL";
    if (ratio >= budget.cleanup_ratio) return "CLEANUP";
    if (ratio >= budget.warning_ratio) return "WARNING";
    return "NORMAL";
}

Budget load_budget(const std::string& path) {
    Budget budget;
    auto document = alttech::config::load_yaml(path);
    if (!document) {
        std::cerr << "warning: cannot read " << path << " (" << document.error().message
                  << "); using built-in budget defaults\n";
        return budget;
    }
    auto parsed = alttech::config::parse_analytics_config(*document);
    if (!parsed) {
        std::cerr << "warning: " << path << " is not a valid service configuration ("
                  << parsed.error().message << "); using built-in budget defaults\n";
        return budget;
    }
    budget.hard_limit = parsed->total_database_hard_limit_bytes;
    budget.soft_limit = parsed->analytics_soft_limit_bytes;
    budget.warning_ratio = parsed->analytics_warning_ratio;
    budget.cleanup_ratio = parsed->analytics_cleanup_ratio;
    budget.critical_ratio = parsed->analytics_critical_ratio;
    return budget;
}

int64_t analytics_bytes(pqxx::transaction_base& txn) {
    return txn.exec("SELECT alttech.schema_bytes()")[0][0].as<int64_t>();
}

int storage_inspect(pqxx::connection& conn, const Budget& budget, bool record) {
    pqxx::work txn(conn);
    const int64_t db_bytes =
        txn.exec("SELECT pg_database_size(current_database())")[0][0].as<int64_t>();
    const int64_t alttech = analytics_bytes(txn);
    const int64_t public_bytes =
        txn.exec("SELECT alttech.public_schema_bytes()")[0][0].as<int64_t>();
    const char* state = budget_state(budget, alttech, db_bytes);

    std::cout << "AltTech storage\n";
    std::cout << "  total database   : " << human_bytes(db_bytes) << " of "
              << human_bytes(budget.hard_limit) << " shared hard limit\n";
    std::cout << "  AltSpotter (pub) : " << human_bytes(public_bytes) << " (read-only)\n";
    std::cout << "  AltTech (alttech): " << human_bytes(alttech) << " of "
              << human_bytes(budget.soft_limit) << " allocation ("
              << (100.0 * static_cast<double>(alttech) / static_cast<double>(budget.soft_limit))
              << "%)\n";
    std::cout << "  budget state     : " << state << "\n";

    std::cout << "\n  Largest AltTech relations:\n";
    for (const auto& r : txn.exec(
             "SELECT n.nspname||'.'||c.relname, pg_total_relation_size(c.oid) "
             "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
             "WHERE n.nspname = 'alttech' AND c.relkind IN ('r','p') "
             "ORDER BY pg_total_relation_size(c.oid) DESC LIMIT 15")) {
        std::cout << "    " << human_bytes(r[1].as<int64_t>()) << "\t" << r[0].as<std::string>()
                  << "\n";
    }

    if (record) {
        // Growth is measured, never estimated: it is the observed change since
        // the previous snapshot and stays NULL until there is one to compare to.
        txn.exec_params(
            "WITH previous AS ("
            "  SELECT captured_at, analytics_bytes FROM alttech.storage_budget_snapshots "
            "  ORDER BY captured_at DESC LIMIT 1), "
            "growth AS ("
            "  SELECT CASE WHEN p.captured_at IS NULL "
            "               OR EXTRACT(EPOCH FROM (now() - p.captured_at)) < 3600 THEN NULL "
            "         ELSE ($3 - p.analytics_bytes) * 86400.0 "
            "              / EXTRACT(EPOCH FROM (now() - p.captured_at)) END AS per_day "
            "  FROM (SELECT 1) one LEFT JOIN previous p ON true) "
            "INSERT INTO alttech.storage_budget_snapshots "
            "(db_bytes, altspotter_schema_bytes, analytics_bytes, analytics_soft_limit_bytes, "
            " hard_limit_bytes, state, daily_growth_bytes, projected_7d_bytes, "
            " projected_30d_bytes) "
            "SELECT $1, $2, $3, $4, $5, $6, growth.per_day::bigint, "
            "       ($3 + 7 * growth.per_day)::bigint, ($3 + 30 * growth.per_day)::bigint "
            "FROM growth",
            db_bytes, public_bytes, alttech, budget.soft_limit, budget.hard_limit,
            std::string(state));
        txn.exec(
            "INSERT INTO alttech.storage_table_statistics "
            "(table_name, row_count_estimate, table_bytes, index_bytes, toast_bytes) "
            "SELECT n.nspname||'.'||c.relname, c.reltuples::bigint, "
            "       pg_table_size(c.oid) - COALESCE(pg_total_relation_size(c.reltoastrelid), 0), "
            "       pg_indexes_size(c.oid), "
            "       COALESCE(pg_total_relation_size(c.reltoastrelid), 0) "
            "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
            "WHERE n.nspname = 'alttech' AND c.relkind IN ('r','p') "
            "ON CONFLICT DO NOTHING");
        std::cout << "\n  recorded storage snapshot (state=" << state << ")\n";
    }
    txn.commit();
    return 0;
}

struct PartitionDrop {
    std::string parent;
    std::string partition;
    std::string upper_bound;
    bool attached = true;   // false: a table an earlier run detached but did not drop
};

// Session settings for one detach or drop, restored however it ends.
class SessionTimeouts {
public:
    explicit SessionTimeouts(pqxx::connection& conn) : conn_(conn) {
        pqxx::nontransaction nt(conn_);
        // The wait is the mechanism here, so no lock_timeout; the statement
        // timeout only bounds a wait that would otherwise be unbounded.
        nt.exec("SET lock_timeout = 0");
        nt.exec("SET statement_timeout = '15min'");
    }
    ~SessionTimeouts() {
        try {
            pqxx::nontransaction nt(conn_);
            nt.exec("RESET lock_timeout");
            nt.exec("RESET statement_timeout");
        } catch (...) {
        }
    }
    SessionTimeouts(const SessionTimeouts&) = delete;
    SessionTimeouts& operator=(const SessionTimeouts&) = delete;

private:
    pqxx::connection& conn_;
};

// Removes one partition without ever taking an ACCESS EXCLUSIVE lock on its
// parent.
//
// DROP TABLE on a partition locks the PARENT exclusively, so it had to find a
// moment when no one at all was reading or writing the parent -- and AltML's
// archive backfill keeps a postgres_fdw cursor open on the venue vector parents
// for a minute or more at a time, back to back. The oldest vector partitions
// were deferred on every hourly run from 00:05 to 10:04 on 2026-09-25 (eleven
// in a row), and under budget pressure that is how the disk fills.
//
// DETACH PARTITION ... CONCURRENTLY takes only SHARE UPDATE EXCLUSIVE on the
// parent, which neither writers (ROW EXCLUSIVE) nor readers (ACCESS SHARE)
// conflict with. It marks the partition as leaving, waits for the transactions
// already using the parent to finish, and from then on no new query can reach
// the partition through the parent. The DROP that follows locks only the
// detached table, which nothing references any more. A run interrupted between
// the two leaves either a detach-pending partition (FINALIZE completes it) or
// a detached table (dropped by name on the next run): both are picked up.
void detach_and_drop(pqxx::connection& conn, const PartitionDrop& d) {
    SessionTimeouts timeouts(conn);
    pqxx::nontransaction nt(conn);
    if (d.attached) {
        const auto state = nt.exec_params(
            "SELECT bool_or(inhdetachpending) FROM pg_inherits WHERE inhrelid = to_regclass($1)",
            d.partition);
        if (!state[0][0].is_null()) {
            nt.exec("ALTER TABLE " + d.parent + " DETACH PARTITION " + d.partition +
                    (state[0][0].as<bool>() ? " FINALIZE" : " CONCURRENTLY"));
        }
    }
    nt.exec("DROP TABLE IF EXISTS " + d.partition);
}

int retention_run(pqxx::connection& conn, const Budget& budget, bool apply) {
    {
        pqxx::nontransaction lock(conn);
        lock.exec_params("SELECT pg_advisory_lock($1)", kRetentionLockKey);
    }
    pqxx::work txn(conn);
    const int64_t before = analytics_bytes(txn);
    const int64_t db_before =
        txn.exec("SELECT pg_database_size(current_database())")[0][0].as<int64_t>();
    const std::string state = budget_state(budget, before, db_before);

    std::cout << "AltTech retention " << (apply ? "(apply)" : "(dry run)") << ", budget state "
              << state << "\n";

    const auto policies = txn.exec(
        "SELECT table_name, target_keep::text, minimum_keep::text "
        "FROM alttech.retention_policies ORDER BY cleanup_priority");

    std::vector<PartitionDrop> drops;
    std::set<std::string> listed;
    const auto collect = [&](const std::string& parent, const std::string& keep) {
        const auto exists = txn.exec_params("SELECT to_regclass($1) IS NOT NULL", parent);
        if (!exists[0][0].as<bool>()) return;
        for (const auto& part : txn.exec_params(
                 "SELECT partition_name, upper_bound::text "
                 "FROM alttech.partition_bounds($1::regclass) "
                 "WHERE upper_bound < now() - $2::interval ORDER BY upper_bound",
                 parent, keep)) {
            if (!listed.insert(part[0].as<std::string>()).second) continue;
            drops.push_back({parent, part[0].as<std::string>(), part[1].as<std::string>(), true});
        }
        // Day tables of this parent that are no longer partitions of it: what
        // an interrupted run detached and did not get to drop.
        for (const auto& t : txn.exec_params(
                 "SELECT n.nspname || '.' || c.relname, "
                 "       (to_date(right(c.relname, 8), 'YYYYMMDD') + 1)::timestamptz::text "
                 "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
                 "WHERE n.nspname = 'alttech' AND c.relkind = 'r' AND NOT c.relispartition "
                 "  AND c.relname ~ ('^' || (SELECT relname FROM pg_class "
                 "                           WHERE oid = $1::regclass) || '_[0-9]{8}$') "
                 "  AND (to_date(right(c.relname, 8), 'YYYYMMDD') + 1)::timestamptz "
                 "      < now() - $2::interval",
                 parent, keep)) {
            if (!listed.insert(t[0].as<std::string>()).second) continue;
            drops.push_back({parent, t[0].as<std::string>(), t[1].as<std::string>(), false});
        }
    };

    // Stage 1: ordinary retention, at each policy's target_keep.
    for (const auto& policy : policies) {
        collect(policy[0].as<std::string>(), policy[1].as<std::string>());
    }

    // Stage 2: budget pressure. Entered only when the schema is at or above the
    // cleanup ratio of its allocation, and never cuts below minimum_keep.
    const bool pressure = static_cast<double>(before) >=
                          budget.cleanup_ratio * static_cast<double>(budget.soft_limit);
    if (pressure) {
        std::cout << "  budget pressure: " << human_bytes(before) << " of "
                  << human_bytes(budget.soft_limit) << " allocation\n";
        for (const auto& policy : policies) {
            collect(policy[0].as<std::string>(), policy[2].as<std::string>());
        }
    }

    txn.commit();

    // Each partition leaves on its own, outside any transaction the run holds
    // open (see detach_and_drop). It used to be DROP TABLE in ONE transaction
    // with a savepoint per partition: a released savepoint keeps its locks
    // until the top-level commit, so the run held the parent-exclusive lock of
    // every table it had already visited while it queued on the next. A feature
    // task writes vectors and projection in one transaction, in the opposite
    // order, and the two closed a cycle; the writer, having waited longer, was
    // the one the deadlock detector cancelled (00:05, 04:02, 09:00 on
    // 2026-09-25).
    int dropped = 0;
    int deferred = 0;
    for (const auto& drop : drops) {
        std::cout << "  " << (apply ? "drop " : "would drop ") << drop.partition
                  << (drop.attached ? "" : " (detached earlier)") << " (upper bound "
                  << drop.upper_bound << ")\n";
        if (!apply) {
            ++dropped;
            continue;
        }
        try {
            detach_and_drop(conn, drop);
            ++dropped;
        } catch (const pqxx::sql_error& e) {
            // 57014 is the statement timeout: a transaction on the parent that
            // has been open for a quarter of an hour. The partition is left
            // detach-pending or detached, and the next run finishes it.
            if (e.sqlstate() != "57014" &&
                !alttech::persistence::is_lock_contention(e.sqlstate())) {
                throw;
            }
            std::cout << "    not finished (" << e.sqlstate() << "); the next run completes it\n";
            ++deferred;
        }
    }

    // Stage 3: the operational tables that are not partitioned. They are
    // bounded by row age with a plain DELETE; the space stays with the table
    // for reuse (autovacuum), which is what a table of constant size needs.
    // analytics_computation_runs alone had reached 7.3 M rows and 1.7 GB in
    // three weeks with nothing bounding it.
    pqxx::work ops(conn);
    struct OpsTable { const char* table; const char* time_column; const char* keep; };
    static constexpr OpsTable kOpsTables[] = {
        {"alttech.analytics_computation_runs", "started_at", "14 days"},
        {"alttech.retention_runs", "started_at", "180 days"},
        {"alttech.storage_budget_snapshots", "captured_at", "180 days"},
    };
    int64_t ops_rows = 0;
    for (const auto& t : kOpsTables) {
        const auto exists = ops.exec_params("SELECT to_regclass($1) IS NOT NULL", t.table);
        if (!exists[0][0].as<bool>()) continue;
        if (!apply) {
            const auto n = ops.exec(std::string("SELECT count(*) FROM ") + t.table + " WHERE " +
                                    t.time_column + " < now() - interval '" + t.keep + "'");
            std::cout << "  would delete " << n[0][0].as<int64_t>() << " row(s) from " << t.table
                      << " older than " << t.keep << "\n";
            continue;
        }
        const auto r = ops.exec(std::string("DELETE FROM ") + t.table + " WHERE " + t.time_column +
                                " < now() - interval '" + t.keep + "'");
        if (r.affected_rows() > 0) {
            std::cout << "  deleted " << r.affected_rows() << " row(s) from " << t.table
                      << " older than " << t.keep << "\n";
        }
        ops_rows += static_cast<int64_t>(r.affected_rows());
    }

    if (apply) {
        const int64_t after = analytics_bytes(ops);
        ops.exec_params(
            "INSERT INTO alttech.retention_runs (finished_at, mode, budget_state, "
            "analytics_bytes_before, analytics_bytes_after, db_bytes_before, db_bytes_after, "
            "partitions_dropped) VALUES (now(), 'manual', $1, $2, $3, $4, "
            "pg_database_size(current_database()), $5)",
            state, before, after, db_before, dropped);
        std::cout << "  dropped " << dropped << " partition(s)";
        if (deferred > 0) std::cout << ", deferred " << deferred;
        std::cout << "; alttech " << human_bytes(before) << " -> " << human_bytes(after) << "\n";
    } else {
        std::cout << "  " << dropped << " partition(s) eligible\n";
    }
    ops.commit();
    {
        pqxx::nontransaction unlock(conn);
        unlock.exec_params("SELECT pg_advisory_unlock($1)", kRetentionLockKey);
    }
    return 0;
}

int cross_venue_inspect(pqxx::connection& conn, int64_t pair) {
    pqxx::work txn(conn);
    const std::string filter = pair > 0 ? " AND canonical_pair_id = " + std::to_string(pair) : "";
    std::cout << "Cross-venue quality over the last 24h\n";
    for (const auto& r : txn.exec(
             "SELECT quality_status, count(*) FROM alttech.cross_venue_feature_vectors "
             "WHERE bar_open_time > now() - interval '24 hours'" + filter +
             " GROUP BY quality_status ORDER BY quality_status")) {
        std::cout << "  " << r[0].as<std::string>() << " : " << r[1].as<int64_t>() << "\n";
    }
    std::cout << "\nLatest cross-venue snapshots\n";
    for (const auto& r : txn.exec(
             "SELECT canonical_pair_id, bar_open_time, quality_status, consensus_mid_price, "
             "bitget_mid_deviation_bps FROM alttech.cross_venue_snapshots "
             "WHERE true" + filter + " ORDER BY bar_open_time DESC LIMIT 10")) {
        std::cout << "  pair " << r[0].as<int64_t>() << " @ " << r[1].as<std::string>() << "  "
                  << r[2].as<std::string>()
                  << "  mid=" << (r[3].is_null() ? "-" : r[3].as<std::string>())
                  << "  bitget_dev_bps=" << (r[4].is_null() ? "-" : r[4].as<std::string>())
                  << "\n";
    }
    std::cout << "\nGlobal regime distribution over the last 24h\n";
    for (const auto& r : txn.exec(
             "SELECT primary_regime, count(*), round(avg(venue_agreement)::numeric, 3) "
             "FROM alttech.global_regimes "
             "WHERE bar_open_time > now() - interval '24 hours'" + filter +
             " GROUP BY primary_regime ORDER BY 2 DESC")) {
        std::cout << "  " << r[0].as<std::string>() << " : " << r[1].as<int64_t>()
                  << " (mean agreement " << r[2].as<std::string>() << ")\n";
    }
    std::cout << "\nActive consensus zones\n";
    for (const auto& r : txn.exec(
             "SELECT canonical_pair_id, kind, round(normalized_center::numeric, 6), "
             "supporting_venue_count, round(confidence::numeric, 3) "
             "FROM alttech.consensus_structure_zones "
             "WHERE lifecycle IN ('CANDIDATE','ACTIVE','TESTED')" + filter +
             " ORDER BY canonical_pair_id, normalized_center LIMIT 20")) {
        std::cout << "  pair " << r[0].as<int64_t>() << "  " << r[1].as<std::string>()
                  << "  center " << r[2].as<std::string>() << "  venues " << r[3].as<int>()
                  << "  confidence " << r[4].as<std::string>() << "\n";
    }
    txn.commit();
    return 0;
}

int alignment_inspect(pqxx::connection& conn, int64_t pair) {
    pqxx::work txn(conn);
    const std::string filter = pair > 0 ? " AND canonical_pair_id = " + std::to_string(pair) : "";
    std::cout << "Alignment status over the last 24h\n";
    for (const auto& r : txn.exec(
             "SELECT alignment_status, count(*), "
             "round(max(max_cross_venue_skew_ms)::numeric, 1) "
             "FROM alttech.venue_alignment_state "
             "WHERE canonical_open_time > now() - interval '24 hours'" + filter +
             " GROUP BY alignment_status ORDER BY alignment_status")) {
        std::cout << "  " << r[0].as<std::string>() << " : " << r[1].as<int64_t>()
                  << " (max skew " << (r[2].is_null() ? "-" : r[2].as<std::string>())
                  << " ms)\n";
    }
    txn.commit();
    return 0;
}

void usage() {
    std::cout << "usage: alttech-analytics <storage-inspect|retention-run|cross-venue-inspect|"
                 "alignment-inspect>\n"
                 "                         [--apply] [--record] [--pair N] [--dsn DSN] "
                 "[--config FILE]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        usage();
        return 0;
    }

    std::string dsn;
    std::string config_path = "/etc/alttech/alttech.yaml";
    bool apply = false;
    bool record = false;
    int64_t pair = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dsn" && i + 1 < argc) dsn = argv[++i];
        else if (arg == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (arg == "--apply") apply = true;
        else if (arg == "--record") record = true;
        else if (arg == "--pair" && i + 1 < argc) pair = std::atoll(argv[++i]);
        else if (arg == "--help" || arg == "-h") { usage(); return 0; }
        else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }
    if (dsn.empty()) {
        if (const char* env = std::getenv("ALTTECH_POSTGRES_DSN")) dsn = env;
    }
    if (dsn.empty()) {
        std::cerr << "no DSN: use --dsn or ALTTECH_POSTGRES_DSN\n";
        return 2;
    }

    try {
        pqxx::connection conn(dsn);
        {
            pqxx::nontransaction setup(conn);
            setup.exec("SET TIME ZONE 'UTC'");
        }
        if (command == "storage-inspect") {
            return storage_inspect(conn, load_budget(config_path), record);
        }
        if (command == "retention-run") {
            return retention_run(conn, load_budget(config_path), apply);
        }
        if (command == "cross-venue-inspect") return cross_venue_inspect(conn, pair);
        if (command == "alignment-inspect") return alignment_inspect(conn, pair);
        std::cerr << "unknown subcommand: " << command << "\n";
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "alttech-analytics error: " << e.what() << "\n";
        return 1;
    }
}
