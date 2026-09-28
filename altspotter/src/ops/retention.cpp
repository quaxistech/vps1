// altspotter retention-run: one-shot retention pass (partition drop) + budget
// report. Reuses the same logic the in-process retention_manager runs.
//
//   altspotter retention-run [--dsn DSN] [--config altspotter.yaml] [--dry-run]
//   DSN fallback: ALTSPOTTER_POSTGRES_DSN.

#include "ops/ops.hpp"
#include "config/config.hpp"
#include "logging/logger.hpp"
#include "persistence/pg.hpp"
#include "persistence/retention.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace altspotter::ops {

int cmd_retention(int argc, char** argv) {
    std::string dsn;
    std::string config_path;
    bool dry_run = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dsn" && i + 1 < argc) {
            dsn = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: altspotter retention-run [--dsn DSN] [--config FILE] [--dry-run]\n";
            return 0;
        }
    }
    if (dsn.empty()) {
        if (const char* env = std::getenv("ALTSPOTTER_POSTGRES_DSN")) dsn = env;
    }
    altspotter::log::init("altspotter-retention", "cli", "info");
    if (dsn.empty()) {
        std::cerr << "no DSN: use --dsn or ALTSPOTTER_POSTGRES_DSN\n";
        return 2;
    }

    persistence::RetentionConfig rcfg;
    if (!config_path.empty()) {
        if (auto doc = config::load_yaml(config_path); doc) {
            const auto s = (*doc)["storage"];
            if (s && s.IsMap()) {
                rcfg.hard_limit_bytes =
                    config::opt_int(s, "hard_database_limit_bytes", rcfg.hard_limit_bytes);
                rcfg.warning_ratio = config::opt_double(s, "warning_ratio", rcfg.warning_ratio);
                rcfg.cleanup_ratio = config::opt_double(s, "cleanup_ratio", rcfg.cleanup_ratio);
                rcfg.critical_ratio = config::opt_double(s, "critical_ratio", rcfg.critical_ratio);
            }
        }
    }

    persistence::PgPool pool(dsn, 2);
    auto budget = persistence::measure_budget(pool, rcfg);
    if (!budget) {
        std::cerr << "budget measurement failed: " << budget.error().message << "\n";
        return 1;
    }
    std::cout << "database size : " << budget->db_bytes / (1024 * 1024) << " MiB\n"
              << "hard limit    : " << budget->hard_limit_bytes / (1024 * 1024) << " MiB\n"
              << "usage ratio   : " << static_cast<int>(budget->ratio * 100) << "%\n"
              << "budget state  : " << budget->state << "\n"
              << "persistence   : " << persistence_mode_label(budget->mode) << "\n";

    if (auto c = persistence::ensure_partitions(pool); c) {
        std::cout << "partitions ensured: " << *c << "\n";
    }

    const bool pressure = budget->state == "CLEANUP" || budget->state == "CRITICAL" ||
                          budget->state == "HARD_LIMIT";
    auto outcome = persistence::apply_retention(pool, rcfg, pressure, dry_run);
    if (!outcome) {
        std::cerr << "retention failed: " << outcome.error().message << "\n";
        return 1;
    }
    std::cout << (dry_run ? "would drop " : "dropped ") << outcome->partitions_dropped
              << " partition(s)\n";
    for (const auto& p : outcome->dropped) std::cout << "  - " << p << "\n";
    if (!dry_run && outcome->bytes_after > 0) {
        std::cout << "database size after: " << outcome->bytes_after / (1024 * 1024) << " MiB\n";
    }
    return 0;
}

}  // namespace altspotter::ops
