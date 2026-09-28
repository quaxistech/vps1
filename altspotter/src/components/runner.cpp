// The `altspotter run` orchestrator: one process, many market-data components.
//
// Each component runs on its own supervised thread and keeps its own NATS
// connection(s) and PostgreSQL pool, so a fault in one never blocks another.
// A single ShutdownToken coordinates a clean stop and a single sd_notify
// reports readiness. A component that exits without a shutdown being requested
// (a transient dependency outage at start-up) is restarted with backoff.

#include "components/runner.hpp"

#include "components/components.hpp"
#include "config/config.hpp"
#include "core/backoff.hpp"
#include "core/readiness.hpp"
#include "core/shutdown.hpp"
#include "logging/logger.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace altspotter {

namespace {

struct ComponentSpec {
    const char* section;       // key in the unified config
    const char* service_name;  // kept for logs and the Envelope `source_service` field
    std::function<int(config::ServiceContext)> fn;
};

// Run one component to completion; on a non-shutdown return, back off and
// restart. The restart delay grows exponentially (5s..5min with jitter) so a
// permanently failing component (e.g. bad config section) cannot spam a
// restart every 5 seconds forever; a component that ran healthily for a while
// resets the backoff, matching systemd's Restart= behaviour.
void supervise(ComponentSpec spec, config::CommonConfig common, YAML::Node section,
               YAML::Node root) {
    auto& sd = ShutdownToken::instance();
    Backoff restart_backoff{std::chrono::seconds(5), std::chrono::minutes(5)};
    while (!sd.stop_requested()) {
        config::ServiceContext ctx;
        ctx.common = common;
        ctx.service = section;
        ctx.root = root;
        ctx.service_name = spec.service_name;
        const auto started = std::chrono::steady_clock::now();
        try {
            spec.fn(std::move(ctx));
        } catch (const std::exception& e) {
            log::error("component_crashed",
                       {{"component", spec.service_name}, {"message", e.what()}});
        }
        if (sd.stop_requested()) break;
        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(1)) {
            restart_backoff.reset();
        }
        const auto delay = restart_backoff.next();
        log::warn("component_restart",
                  {{"component", spec.service_name},
                   {"duration_ms", delay.count()},
                   {"message", "component exited without shutdown; restarting after backoff"}});
        if (sd.wait_for(delay)) break;
    }
    log::info("component_stopped", {{"component", spec.service_name}});
}

}  // namespace

int run_service(const std::string& config_path, const std::string& common_path,
                const std::vector<std::string>& only) {
    auto common = config::load_common(common_path);
    if (!common) {
        std::fprintf(stderr, "config error: %s\n", common.error().message.c_str());
        return 2;
    }
    auto root = config::load_yaml(config_path);
    if (!root) {
        std::fprintf(stderr, "config error: %s\n", root.error().message.c_str());
        return 2;
    }

    log::init("altspotter", common->instance_id, common->log_level);
    ShutdownToken::instance().install_signal_handlers();
    log::info("service_starting",
              {{"environment", common->environment}, {"config", config_path}});

    const std::vector<ComponentSpec> specs = {
        {"instrument_catalog", "instrument_catalog", components::instrument_catalog},
        {"market_gateway", "market_gateway", components::market_gateway},
        {"microstructure_collector", "microstructure_collector",
         components::microstructure_collector},
        {"market_history", "market_history", components::market_history},
        {"storage_writer", "storage_writer", components::storage_writer},
        {"bar_aggregation", "bar_aggregation", components::bar_aggregation},
        {"data_integrity", "data_integrity", components::data_integrity},
        {"binance_usdm_runtime", "binance_usdm_runtime", components::binance_usdm_runtime},
        {"bybit_runtime", "bybit_runtime", components::bybit_runtime},
        {"okx_runtime", "okx_runtime", components::okx_runtime},
        {"bitget_context_runtime", "bitget_context_runtime", components::bitget_context},
        {"microstructure_aggregation", "microstructure_aggregation",
         components::microstructure_aggregation},
        {"retention_manager", "retention_manager", components::retention_manager},
    };

    // A component section maps to a venue enable flag (exchanges.<venue>.enabled).
    // A disabled venue's runtime is not started; the four-venue intersection
    // then never marks its pairs eligible, so nothing else observes them either.
    const auto venue_of = [](const std::string& section) -> const char* {
        if (section == "binance_usdm_runtime") return "binance_usdm";
        if (section == "bybit_runtime") return "bybit";
        if (section == "okx_runtime") return "okx";
        if (section == "bitget_context_runtime") return "bitget";
        return nullptr;
    };
    const auto venue_enabled = [&](const char* venue) {
        const auto ex = (*root)["exchanges"];
        if (!ex || !ex[venue]) return true; // default on when unspecified
        return config::opt_bool(ex[venue], "enabled", true);
    };

    const auto selected = [&](const char* section) {
        return only.empty() ||
               std::find(only.begin(), only.end(), section) != only.end();
    };
    // Reject unknown --only names so a typo fails loudly instead of running nothing.
    for (const auto& name : only) {
        const bool known = std::any_of(specs.begin(), specs.end(), [&](const ComponentSpec& s) {
            return name == s.section;
        });
        if (!known) {
            std::fprintf(stderr, "unknown component: %s\n", name.c_str());
            return 2;
        }
    }

    std::vector<std::thread> threads;
    threads.reserve(specs.size());
    int enabled = 0;
    for (const auto& spec : specs) {
        if (!selected(spec.section)) continue;
        if (const char* v = venue_of(spec.section); v != nullptr && !venue_enabled(v)) {
            log::info("venue_runtime_disabled", {{"component", spec.service_name}});
            continue;
        }
        // Deep-copy the section AND the full root: yaml-cpp nodes share one
        // document tree (and its memory pool) and are not thread-safe, so each
        // component thread must own an independent tree. Components that need
        // cross-section config (catalog, retention) read their own root clone.
        YAML::Node raw = (*root)[spec.section];
        YAML::Node section = raw ? YAML::Clone(raw) : YAML::Node(YAML::NodeType::Map);
        YAML::Node root_copy = YAML::Clone(*root);
        threads.emplace_back(supervise, spec, *common, section, root_copy);
        ++enabled;
    }

    // The process is up and every component is self-healing; report readiness so
    // systemd (Type=notify) considers the unit started.
    notify_ready();
    notify_status(("running " + std::to_string(enabled) + " market-data components").c_str());
    log::info("service_ready", {{"components", enabled}});

    auto& sd = ShutdownToken::instance();

    // systemd watchdog. `Restart=on-failure` only catches a process that EXITS;
    // a deadlocked one — every component thread blocked on a connection pool
    // that never returns, say — stays "active" forever and silently stops
    // collecting. The heartbeat is emitted from this loop, which is the thread
    // that would stop running in exactly that case.
    //
    // Fed at half the configured interval, per systemd's own recommendation, so
    // one missed tick is not fatal. Disabled unless the unit sets WatchdogSec,
    // so nothing changes where it is not enabled.
    const uint64_t wd_usec = watchdog_interval_usec();
    const auto wd_period = std::chrono::microseconds(wd_usec / 2);
    auto next_ping = std::chrono::steady_clock::now();
    if (wd_usec > 0) {
        log::info("watchdog_enabled", {{"interval_ms", static_cast<int64_t>(wd_usec / 1000)}});
    }

    while (!sd.wait_for(std::chrono::seconds(1))) {
        if (wd_usec == 0) continue;
        const auto now = std::chrono::steady_clock::now();
        if (now < next_ping) continue;
        notify_watchdog();
        next_ping = now + wd_period;
    }

    notify_stopping();
    log::info("service_stopping", {});
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    log::info("service_stopped", {});
    return 0;
}

}  // namespace altspotter
