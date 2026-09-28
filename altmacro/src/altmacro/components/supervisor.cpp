#include "altmacro/components/supervisor.hpp"

#include "altmacro/core/logger.hpp"
#include "altmacro/core/readiness.hpp"
#include "altmacro/core/shutdown.hpp"
#include "altmacro/core/time.hpp"

#include <cstdlib>

namespace altmacro::components {
namespace {
constexpr int64_t kHourMs = 3'600'000;
constexpr int64_t kDayMs = 86'400'000;
} // namespace

Result<void> Supervisor::run() {
    auto& shutdown = ShutdownToken::instance();
    shutdown.install_signal_handlers();

    // The watchdog is fed from inside every blocking wait, not merely between
    // poll cycles.
    //
    // A single poll pass walks every binding, and a throttled provider can hold
    // one of them for minutes -- the backoff climbs 5, 10, 20, 40 seconds and
    // there are eight bindings. Pinging only after the pass finished meant the
    // deadline expired mid-pass: systemd killed and restarted the service
    // nineteen times in a row, each restart began again at the first binding,
    // and `systemctl is-active` said `active` throughout while only one series
    // ever received data.
    //
    // The stop predicate is already consulted inside every rate-limit wait and
    // every backoff, so hanging the heartbeat on it reaches all of them without
    // threading a second callback through the provider interface.
    int64_t last_ping = 0;
    const auto should_stop = [&]() -> bool {
        const int64_t now = timeutil::now_ms();
        if (now - last_ping >= 5'000) {
            last_ping = now;
            notify_watchdog();
        }
        return shutdown.stop_requested();
    };

    const char* pw = std::getenv("ALTMACRO_DB_PASSWORD");
    if (pw == nullptr) {
        return fail(ErrorCode::config_error,
                    "ALTMACRO_DB_PASSWORD is not set; the database password is supplied by the "
                    "runtime environment and is never read from configuration");
    }
    pool_ = std::make_unique<persistence::PgPool>(cfg_.dsn(pw), cfg_.database.pool_size);
    store_ = std::make_unique<data::Store>(*pool_);

    auto bindings = store_->load_bindings();
    if (!bindings) return std::unexpected(bindings.error());

    auto registry = providers::Registry::build(cfg_, *bindings);
    if (!registry) return std::unexpected(registry.error());
    registry_ = std::make_unique<providers::Registry>(std::move(*registry));

    log::info("registry_resolved",
              {{"active_bindings", registry_->active().size()},
               {"rejected_bindings", registry_->rejected().size()}});
    for (const auto& r : registry_->rejected()) {
        log::warn("binding_not_served", {{"instrument", r.instrument_code},
                                         {"provider", r.provider_code}, {"reason", r.reason}});
    }
    if (registry_->active().empty()) {
        return fail(ErrorCode::config_error,
                    "no binding can be served; there would be nothing to poll");
    }

    if (cfg_.nats.enabled) {
        auto n = messaging::NatsClient::connect(cfg_.nats.url, "altmacro-" + cfg_.instance_id);
        if (!n) {
            // Publication is how downstream sees this data, so its absence is
            // reported rather than tolerated silently — but the service still
            // runs, because the database remains the system of record.
            log::error("nats_connect_failed", {{"url", cfg_.nats.url},
                                               {"error", n.error().message}});
        } else {
            nats_ = std::move(*n);
            publisher_ = std::make_unique<Publisher>(cfg_.nats, nats_.get());
        }
    }

    Poller poller(cfg_, *registry_, *store_, publisher_.get());

    notify_ready();
    log::info("service_ready", {{"config", cfg_.redacted_summary()}});

    int64_t last_partition_sweep = 0;
    while (!shutdown.stop_requested()) {
        const int64_t now = timeutil::now_ms();

        // Hourly, not only at startup. AltTech once advanced its partitions
        // solely on start, ran past its horizon, and took the whole stack down
        // at midnight UTC with every unit still reported active. The same
        // mistake is not repeated here, and a failure is logged rather than
        // fatal.
        if (now - last_partition_sweep >= kHourMs) {
            auto created = store_->ensure_partitions(now - kDayMs, now + 31 * kDayMs);
            if (created) {
                if (*created > 0) log::info("partitions_advanced", {{"created", *created}});
            } else {
                log::error("partition_maintenance_failed", {{"error", created.error().message}});
            }
            last_partition_sweep = now;
        }

        // Also pings the watchdog between bindings, via should_stop.
        const auto tick = poller.poll_once(should_stop);
        if (tick.bars_written || tick.reference_written || tick.published ||
            tick.provider_failures) {
            log::info("poll_tick", {{"bars", tick.bars_written},
                                    {"reference", tick.reference_written},
                                    {"published", tick.published},
                                    {"provider_failures", tick.provider_failures}});
        }

        notify_watchdog();
        // Short sleeps so a stop is honoured promptly; the poller itself
        // decides what is actually due.
        shutdown.wait_for(std::chrono::seconds(15));
    }

    log::info("service_stopping", {});
    notify_stopping();
    return {};
}

} // namespace altmacro::components
