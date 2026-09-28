#pragma once

#include "altmacro/core/config.hpp"
#include "altmacro/core/error.hpp"

#include <string>
#include <vector>

namespace altmacro::cli {

// Runs the service: live polling loop under systemd.
[[nodiscard]] int run_service(const Config& cfg);

// One-shot historical load. Safe to interrupt and safe to repeat.
[[nodiscard]] int run_bootstrap(const Config& cfg, bool scan_gaps);

// Read-only reports: registry, coverage, gaps, provider health.
[[nodiscard]] int run_inspect(const Config& cfg, const std::string& what);

// Creates or updates altmacro's JetStream stream. Idempotent; creates no
// consumers, because downstream services own their own durables.
[[nodiscard]] int run_nats_setup(const Config& cfg);

// Offline checks that need no network and no database.
[[nodiscard]] int run_selftest();

} // namespace altmacro::cli
