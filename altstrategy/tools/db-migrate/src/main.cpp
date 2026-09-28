// altstrategy-db-migrate: ordered, checksummed, advisory-locked PostgreSQL migrations.
//
//   altstrategy-db-migrate --dsn <dsn> --migrations <dir> [--ensure-partitions-only]
//   DSN fallback: ALTSTRATEGY_POSTGRES_DSN environment variable.
//
// Behaviour:
//   * migrations (NNNN_name.sql) run in lexical order, each in its own transaction;
//   * applied versions are recorded in schema_migrations with a SHA-256 checksum;
//   * a mutated already-applied file is a hard error;
//   * pg_advisory_lock serializes concurrent runs;
//   * finally altstrategy_ensure_candle_partitions() covers [now-120d, now+3mo].

#include "altstrategy/common/hash.hpp"
#include "altstrategy/logging/logger.hpp"
#include "altstrategy/time/time.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using altstrategy::log::error;
using altstrategy::log::info;

namespace {

constexpr int64_t kMigrationLockKey = 0x68667421; // "astr"

struct MigrationFile {
    std::string version; // "0001"
    std::string name;    // "core_catalog"
    fs::path path;
    std::string sql;
    std::string checksum;
};

std::vector<MigrationFile> load_migrations(const fs::path& dir) {
    std::vector<MigrationFile> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sql") continue;
        const std::string stem = entry.path().stem().string();
        const auto underscore = stem.find('_');
        if (underscore == std::string::npos) {
            throw std::runtime_error("migration file name must be NNNN_name.sql: " + stem);
        }
        MigrationFile m;
        m.version = stem.substr(0, underscore);
        m.name = stem.substr(underscore + 1);
        m.path = entry.path();
        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        m.sql = ss.str();
        if (m.sql.empty()) throw std::runtime_error("empty migration file: " + stem);
        m.checksum = altstrategy::sha256_hex(m.sql);
        files.push_back(std::move(m));
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.version < b.version; });
    for (size_t i = 1; i < files.size(); ++i) {
        if (files[i].version == files[i - 1].version) {
            throw std::runtime_error("duplicate migration version: " + files[i].version);
        }
    }
    return files;
}

} // namespace

int main(int argc, char** argv) {
    std::string dsn;
    std::string migrations_dir = "db/migrations";
    bool partitions_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dsn" && i + 1 < argc) {
            dsn = argv[++i];
        } else if (arg == "--migrations" && i + 1 < argc) {
            migrations_dir = argv[++i];
        } else if (arg == "--ensure-partitions-only") {
            partitions_only = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: altstrategy-db-migrate [--dsn DSN] [--migrations DIR] "
                         "[--ensure-partitions-only]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }
    if (dsn.empty()) {
        if (const char* env = std::getenv("ALTSTRATEGY_POSTGRES_DSN")) dsn = env;
    }
    altstrategy::log::init("altstrategy-db-migrate", "cli", "info");
    if (dsn.empty()) {
        error("migrate_config_error", {{"message", "no DSN: use --dsn or ALTSTRATEGY_POSTGRES_DSN"}});
        return 2;
    }

    try {
        pqxx::connection conn(dsn);
        {
            pqxx::nontransaction setup(conn);
            setup.exec("SET TIME ZONE 'UTC'");
            setup.exec_params("SELECT pg_advisory_lock($1)", kMigrationLockKey);
        }

        if (!partitions_only) {
            {
                pqxx::work txn(conn);
                txn.exec(
                    "CREATE TABLE IF NOT EXISTS schema_migrations ("
                    " version TEXT PRIMARY KEY,"
                    " name TEXT NOT NULL,"
                    " checksum TEXT NOT NULL,"
                    " applied_at TIMESTAMPTZ NOT NULL DEFAULT now())");
                txn.commit();
            }

            const auto files = load_migrations(migrations_dir);
            int applied = 0;
            for (const auto& m : files) {
                pqxx::work txn(conn);
                const auto existing = txn.exec_params(
                    "SELECT checksum FROM schema_migrations WHERE version = $1", m.version);
                if (!existing.empty()) {
                    const std::string prev = existing[0][0].as<std::string>();
                    if (prev != m.checksum) {
                        error("migration_checksum_mismatch",
                              {{"message", "applied migration file was modified"},
                               {"version", m.version},
                               {"expected", prev},
                               {"actual", m.checksum}});
                        return 1;
                    }
                    continue;
                }
                txn.exec(m.sql);
                txn.exec_params(
                    "INSERT INTO schema_migrations (version, name, checksum) VALUES ($1, $2, $3)",
                    m.version, m.name, m.checksum);
                txn.commit();
                ++applied;
                info("migration_applied", {{"version", m.version}, {"name", m.name}});
            }
            info("migrations_done", {{"applied", applied},
                                     {"total", static_cast<int64_t>(files.size())}});
        }
        // AltStrategy owns only the `altstrategy` schema (§33/§43); it manages no
        // partitioned market tables, so there are no partitions to ensure here.

        pqxx::nontransaction unlock(conn);
        unlock.exec_params("SELECT pg_advisory_unlock($1)", kMigrationLockKey);
        return 0;
    } catch (const std::exception& e) {
        error("migrate_failed", {{"message", e.what()}});
        return 1;
    }
}
