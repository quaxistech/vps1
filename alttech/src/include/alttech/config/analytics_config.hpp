#pragma once

#include "alttech/common/error.hpp"

#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace alttech::config {

struct AnalyticsConfig {
    std::size_t postgres_pool_size = 8;
    // Server-side per-query bound shared by every pooled connection (ms). Keeps a
    // stuck query from wedging a worker; must exceed the slowest legitimate
    // backfill chunk read. 0 disables the bound.
    int64_t postgres_statement_timeout_ms = 120'000;
    YAML::Node feature;
    YAML::Node microstructure;
    YAML::Node structure;
    YAML::Node regime;
    YAML::Node cross_venue;   // top-level cross_venue block (alignment/weighting)
    YAML::Node macro;         // macro / cross-asset block from AltMacro

    // Venue identity. Validated: the primary venue is Bitget and the references
    // are exactly the three USDT-perpetual reference venues; spot is rejected.
    std::string primary_exchange = "bitget";
    std::vector<std::string> reference_exchanges;

    // AltTech's own allocation inside the shared database, and the ceiling for
    // the whole database on the deployment host.
    int64_t total_database_hard_limit_bytes = 20'000'000'000;
    int64_t analytics_soft_limit_bytes = 3'000'000'000;
    double analytics_warning_ratio = 0.80;
    double analytics_cleanup_ratio = 0.90;
    double analytics_critical_ratio = 0.95;
};

// Parses and validates the single service document shared by every module.
alttech::Result<AnalyticsConfig> parse_analytics_config(const YAML::Node& document);

} // namespace alttech::config
