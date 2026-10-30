// alttech-schema: applies the single AltTech schema definition and rolls its
// daily partitions forward.
//
//   alttech-schema [--dsn DSN] [--schema FILE]
//   alttech-schema --ensure-partitions [--days-back N] [--days-ahead N]
//   alttech-schema --check
//   alttech-schema --recreate            (drops schema alttech and rebuilds it)
//
// AltTech has no migration lineage: db/schema.sql is the whole definition. Its
// SHA-256 is recorded in alttech.schema_version, so a database whose schema no
// longer matches the binary is detected instead of silently diverging. An
// AltTech-specific advisory lock serializes concurrent runs; it is deliberately
// not AltSpotter's lock, because the two schemas are independent.
//
// DSN precedence: --dsn, ALTTECH_MIGRATION_DSN, ALTTECH_POSTGRES_DSN.

#include "alttech/common/hash.hpp"
#include "alttech/persistence/maintenance_lock.hpp"
#include "sql_digest.hpp"
#include "alttech/logging/logger.hpp"

#include <pqxx/pqxx>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using alttech::log::error;
using alttech::log::info;

using alttech::tools::schema::normalize_sql;

namespace {

constexpr int64_t kSchemaLockKey = 0x616C7474; // 'altt'

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read schema file: " + path.string());
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string sql = buffer.str();
    if (sql.empty()) throw std::runtime_error("schema file is empty: " + path.string());
    return sql;
}

// The recorded checksum, or an empty string when the schema was never applied.
std::string applied_checksum(pqxx::connection& conn) {
    pqxx::work txn(conn);
    const auto present = txn.exec(
        "SELECT to_regclass('alttech.schema_version') IS NOT NULL");
    if (!present[0][0].as<bool>()) {
        txn.commit();
        return {};
    }
    const auto row = txn.exec("SELECT checksum FROM alttech.schema_version");
    txn.commit();
    return row.empty() ? std::string{} : row[0][0].as<std::string>();
}

// Creating a partition needs an AccessExclusiveLock on the parent, so this
// yields to live analytics writes rather than competing with them; see
// maintenance_lock.hpp.
int ensure_partitions(pqxx::connection& conn, int days_back, int days_ahead) {
    {
        pqxx::work probe(conn);
        const auto present = probe.exec(
            "SELECT to_regclass('alttech.ensure_all_partitions'::text) IS NOT NULL "
            "OR EXISTS (SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
            "           WHERE n.nspname = 'alttech' AND p.proname = 'ensure_all_partitions')");
        probe.commit();
        if (!present[0][0].as<bool>()) return 0;
    }
    // ONE parent per transaction. ensure_all_partitions() walks every parent
    // inside a single transaction, so the busiest table — the one live analytics
    // writes hold RowExclusiveLock on all the time — aborted the whole run, and
    // the four retries queued behind that same table. Every hourly maintenance
    // pass failed while the other parents, which were free, were rolled back
    // with it. Per-parent transactions let contention cost only the table that
    // is actually busy, and it is picked up on the next pass.
    std::vector<std::string> parents;
    {
        pqxx::work list(conn);
        for (const auto& row : list.exec("SELECT unnest(alttech.partitioned_parents())")) {
            parents.push_back(row[0].as<std::string>());
        }
        list.commit();
    }

    int created = 0;
    int contended = 0;
    std::string last_error;
    for (const auto& parent : parents) {
        int attempts = 1;
        const auto done = alttech::persistence::with_maintenance_lock(
            conn,
            [&](pqxx::work& txn) {
                // Never recreate a day retention has already reclaimed. The
                // backward window is a safety margin for late writes, but each
                // family's retention horizon is shorter than the margin for the
                // big tables, so a fixed `days_back` recreated partitions that
                // the next retention pass immediately dropped again — an hourly
                // create/drop churn on exactly the parents live writers hold.
                // Clamp the window to the oldest partition that still exists;
                // a parent with none (a fresh or fully reclaimed table) keeps
                // the full window.
                created += txn.exec_params(
                                  "SELECT alttech.ensure_daily_partitions($1::regclass, "
                                  "GREATEST(now() - make_interval(days => $2), "
                                  "         COALESCE((SELECT min(lower_bound) "
                                  "                   FROM alttech.partition_bounds($1::regclass)), "
                                  "                  now() - make_interval(days => $2))), "
                                  "now() + make_interval(days => $3))",
                                  parent, days_back, days_ahead)[0][0]
                               .as<int>();
            },
            &attempts);
        if (!done) {
            // Retryable means "the table was busy"; anything else is a real
            // fault and must stop the run.
            if (!done.error().retryable) throw std::runtime_error(done.error().message);
            ++contended;
            last_error = done.error().message;
            info("partition_parent_deferred", {{"parent", parent}});
        } else if (attempts > 1) {
            info("partition_lock_contended", {{"parent", parent}, {"attempts", attempts}});
        }
    }
    if (contended > 0 && contended == static_cast<int>(parents.size())) {
        throw std::runtime_error("every partition parent was locked by live writers: " +
                                 last_error);
    }
    if (contended > 0) {
        info("partition_maintenance_partial",
             {{"deferred_parents", contended},
              {"total_parents", static_cast<int>(parents.size())}});
    }
    return created;
}

void usage() {
    std::cout << "usage: alttech-schema [--dsn DSN] [--schema FILE] [--check] [--recreate]\n"
                 "                      [--ensure-partitions] [--days-back N] [--days-ahead N]\n"
                 "                      [--digest] [--upgrade FILE]\n"
                 "\n"
                 "  --digest  print the schema file's checksum and exit, without a database.\n"
                 "            It ignores comments and whitespace, so it answers \"would this\n"
                 "            edit require a rebuild?\" before the edit is deployed.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string dsn;
    fs::path schema_file = "db/schema.sql";
    fs::path upgrade_file;
    bool partitions_only = false;
    bool check_only = false;
    bool recreate = false;
    bool digest_only = false;
    int days_back = 8;
    int days_ahead = 7;   // a week: see feature_module.cpp, partition advance

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dsn" && i + 1 < argc) {
            dsn = argv[++i];
        } else if (arg == "--schema" && i + 1 < argc) {
            schema_file = argv[++i];
        } else if (arg == "--upgrade" && i + 1 < argc) {
            upgrade_file = argv[++i];
        } else if (arg == "--ensure-partitions") {
            partitions_only = true;
        } else if (arg == "--check") {
            check_only = true;
        } else if (arg == "--recreate") {
            recreate = true;
        } else if (arg == "--digest") {
            digest_only = true;
        } else if (arg == "--days-back" && i + 1 < argc) {
            days_back = std::atoi(argv[++i]);
        } else if (arg == "--days-ahead" && i + 1 < argc) {
            days_ahead = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }
    if (digest_only) {
        try {
            std::cout << alttech::sha256_hex(normalize_sql(read_file(schema_file))) << "\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }
    if (days_back < 0 || days_back > 365 || days_ahead < 0 || days_ahead > 365) {
        std::cerr << "--days-back/--days-ahead must be within [0, 365]\n";
        return 2;
    }
    if (!upgrade_file.empty() && (partitions_only || recreate || digest_only)) {
        std::cerr << "--upgrade cannot be combined with schema mutation/digest modes\n";
        return 2;
    }
    if (dsn.empty()) {
        if (const char* env = std::getenv("ALTTECH_MIGRATION_DSN")) dsn = env;
    }
    if (dsn.empty()) {
        if (const char* env = std::getenv("ALTTECH_POSTGRES_DSN")) dsn = env;
    }

    alttech::log::init("alttech-schema", "cli", "info");
    if (dsn.empty()) {
        error("schema_config_error",
              {{"message",
                "no DSN: use --dsn, ALTTECH_MIGRATION_DSN or ALTTECH_POSTGRES_DSN"}});
        return 2;
    }

    try {
        pqxx::connection conn(dsn);
        {
            pqxx::nontransaction setup(conn);
            setup.exec("SET TIME ZONE 'UTC'");
            setup.exec_params("SELECT pg_advisory_lock($1)", kSchemaLockKey);
        }

        int exit_code = 0;
        if (!upgrade_file.empty()) {
            if (applied_checksum(conn).empty()) throw std::runtime_error("base AltTech schema must be installed before forward upgrades");
            const std::string sql = read_file(upgrade_file);
            // Exact bytes: an applied upgrade may never be silently rewritten.
            const std::string checksum = alttech::sha256_hex(sql);
            const std::string name = upgrade_file.filename().string();
            pqxx::work txn(conn);
            txn.exec("CREATE TABLE IF NOT EXISTS alttech.schema_upgrades (name TEXT PRIMARY KEY, checksum TEXT NOT NULL, applied_at TIMESTAMPTZ NOT NULL DEFAULT now())");
            const auto rows = txn.exec_params("SELECT checksum FROM alttech.schema_upgrades WHERE name=$1", name);
            if (!rows.empty()) {
                if (rows[0][0].as<std::string>() != checksum) throw std::runtime_error("applied upgrade checksum mismatch: " + name);
                info("schema_upgrade_current", {{"name", name}, {"checksum", checksum}});
            } else if (check_only) {
                error("schema_upgrade_missing", {{"name", name}});
                exit_code = 1;
            } else {
                txn.exec(sql);
                txn.exec_params("INSERT INTO alttech.schema_upgrades(name,checksum) VALUES($1,$2)", name, checksum);
                info("schema_upgrade_applied", {{"name", name}, {"checksum", checksum}});
            }
            if (check_only) txn.abort(); else txn.commit();
        } else if (partitions_only) {
            const int created = ensure_partitions(conn, days_back, days_ahead);
            info("partitions_ensured", {{"created", created}});
        } else {
            const std::string sql = read_file(schema_file);
            const std::string checksum = alttech::sha256_hex(normalize_sql(sql));
            const std::string current = applied_checksum(conn);

            if (check_only) {
                if (current.empty()) {
                    error("schema_not_applied", {{"expected", checksum}});
                    exit_code = 1;
                } else if (current != checksum) {
                    error("schema_checksum_mismatch",
                          {{"applied", current}, {"expected", checksum}});
                    exit_code = 1;
                } else {
                    info("schema_current", {{"checksum", checksum}});
                }
            } else {
                if (recreate) {
                    pqxx::work drop(conn);
                    drop.exec("DROP SCHEMA IF EXISTS alttech CASCADE");
                    drop.commit();
                    info("schema_dropped", {{"message", "alttech schema removed on request"}});
                } else if (!current.empty() && current != checksum) {
                    // AltTech keeps no migration lineage, so an out-of-date
                    // database cannot be evolved in place: it is rebuilt. The
                    // checksum ignores comments and whitespace (see
                    // sql_digest.hpp), so reaching here means the DDL itself
                    // changed, not the prose describing it.
                    error("schema_checksum_mismatch",
                          {{"applied", current},
                           {"expected", checksum},
                           {"message", "database schema differs from db/schema.sql; "
                                       "re-run with --recreate to rebuild it"}});
                    exit_code = 1;
                }

                if (exit_code == 0) {
                    pqxx::work txn(conn);
                    txn.exec(sql);
                    txn.exec_params(
                        "INSERT INTO alttech.schema_version (singleton, checksum, applied_at) "
                        "VALUES (true, $1, now()) "
                        "ON CONFLICT (singleton) DO UPDATE SET checksum = EXCLUDED.checksum, "
                        "applied_at = EXCLUDED.applied_at",
                        checksum);
                    txn.commit();
                    const int created = ensure_partitions(conn, days_back, days_ahead);
                    info("schema_applied", {{"checksum", checksum},
                                            {"partitions_created", created}});
                }
            }
        }

        pqxx::nontransaction unlock(conn);
        unlock.exec_params("SELECT pg_advisory_unlock($1)", kSchemaLockKey);
        return exit_code;
    } catch (const std::exception& e) {
        error("schema_failed", {{"message", e.what()}});
        return 1;
    }
}
