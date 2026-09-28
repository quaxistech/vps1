#include "alttech/application/module_supervisor.hpp"
#include "alttech/config/analytics_config.hpp"
#include "alttech/modules/cross_venue.hpp"
#include "alttech/modules/macro.hpp"
#include "alttech/modules/feature.hpp"
#include "alttech/modules/microstructure.hpp"
#include "alttech/modules/regime.hpp"
#include "alttech/modules/structure.hpp"

#include "alttech/common/readiness.hpp"
#include "alttech/common/shutdown.hpp"
#include "alttech/config/service_bootstrap.hpp"
#include "alttech/logging/logger.hpp"
#include "alttech/messaging/nats_client.hpp"
#include "alttech/persistence/pg.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Runner = alttech::Result<void> (*)(const YAML::Node&, alttech::persistence::PgPool&,
                                     alttech::messaging::NatsClient&,
                                     alttech::application::ModuleSupervisor&);

void run_guarded(std::string_view name, Runner runner, const YAML::Node& configuration,
                 alttech::persistence::PgPool& database, alttech::messaging::NatsClient& messaging,
                 alttech::application::ModuleSupervisor& supervisor) noexcept {
    try {
        auto result = runner(configuration, database, messaging, supervisor);
        if (!result) {
            supervisor.mark_failed(name, result.error());
        } else if (!alttech::ShutdownToken::instance().stop_requested()) {
            supervisor.mark_failed(
                name, alttech::Error{alttech::ErrorCode::internal_error,
                                 "analytics module exited before process shutdown", false});
        }
    } catch (const std::exception& error) {
        supervisor.mark_failed(
            name, alttech::Error{alttech::ErrorCode::internal_error,
                             std::string("unhandled module exception: ") + error.what(), false});
    } catch (...) {
        supervisor.mark_failed(
            name, alttech::Error{alttech::ErrorCode::internal_error,
                             "unhandled non-standard module exception", false});
    }
}

} // namespace

int main(int argc, char** argv) {
    auto context = alttech::config::bootstrap_service("alttech-service", argc, argv);
    if (!context) {
        std::fprintf(stderr, "bootstrap failed: %s\n", context.error().message.c_str());
        return 2;
    }

    auto configuration = alttech::config::parse_analytics_config(context->service);
    if (!configuration) {
        alttech::log::critical("invalid_analytics_config",
                           {{"message", configuration.error().message}});
        return 2;
    }

    auto messaging = alttech::messaging::NatsClient::connect(context->common.nats_url,
                                                          context->service_name);
    if (!messaging) {
        alttech::log::critical("nats_unavailable", {{"message", messaging.error().message}});
        return 1;
    }

    alttech::persistence::PgPool database(context->common.postgres_dsn,
                                      configuration->postgres_pool_size,
                                      configuration->postgres_statement_timeout_ms);
    // Five supervised in-process modules: feature, microstructure, structure
    // and regime for the primary venue and the per-venue sweeps they drive,
    // plus the cross-venue module.
    alttech::application::ModuleSupervisor supervisor(6);
    std::vector<std::thread> modules;
    modules.reserve(5);

    const auto launch = [&](std::string_view name, Runner runner, const YAML::Node& node) {
        modules.emplace_back(run_guarded, name, runner, node, std::ref(database),
                             std::ref(**messaging), std::ref(supervisor));
    };
    launch("feature", alttech::modules::run_feature, configuration->feature);
    launch("microstructure", alttech::modules::run_microstructure,
           configuration->microstructure);
    launch("structure", alttech::modules::run_structure, configuration->structure);
    launch("regime", alttech::modules::run_regime, configuration->regime);
    launch("cross_venue", alttech::modules::run_cross_venue, configuration->cross_venue);
    launch("macro", alttech::modules::run_macro, configuration->macro);

    auto& shutdown = alttech::ShutdownToken::instance();
    const bool started = supervisor.wait_until_started();
    if (started) {
        alttech::notify_ready();
        alttech::notify_status("all analytics modules ready");
        alttech::log::info("analytics_service_ready", {{"modules", 5}});

        // Liveness cadence: when the manager set WatchdogSec, tick at half the
        // interval (systemd's recommendation) so a silent process hang — not
        // just a crash — is force-restarted. Falls back to the historical 5s
        // tick when no watchdog is configured (local runs, no NOTIFY_SOCKET).
        const uint64_t watchdog_usec = alttech::watchdog_interval_usec();
        std::chrono::milliseconds tick{5000};
        if (watchdog_usec > 0) {
            std::chrono::milliseconds half{static_cast<int64_t>(watchdog_usec / 2000)};
            if (half.count() < 1000) half = std::chrono::milliseconds{1000};
            if (half < tick) tick = half;
        }

        while (!shutdown.wait_for(tick)) {
            if (!(**messaging).connected()) {
                alttech::notify_status("degraded: NATS reconnecting");
            }
            // The database can stop matching this binary while the process is
            // running — a schema dropped, a privilege revoked. That is the same
            // integrity breach the start-up probes fail on, so it must have the
            // same consequence: stop. Retrying forever would leave the service
            // reporting itself healthy while producing nothing, which is worse
            // than being down, because nothing raises an alarm.
            if (alttech::persistence::schema_integrity_lost(
                    database.consecutive_schema_errors())) {
                const alttech::Error error{
                    alttech::ErrorCode::schema_error,
                    "the database no longer matches this binary: the alttech schema or a "
                    "required privilege is gone", false};
                alttech::log::critical("schema_integrity_lost",
                                   {{"message", error.message},
                                    {"consecutive_schema_errors",
                                     database.consecutive_schema_errors()}});
                alttech::notify_status("stopping: alttech schema is unreachable");
                supervisor.mark_failed("database", error);
                shutdown.request_stop();
                break;
            }
            // Only heartbeat while genuinely healthy: no latched fatal failure
            // AND every module still within its liveness deadline. A wedged
            // worker stops beaconing -> all_alive() turns false -> pings stop ->
            // systemd force-restarts. A latched failure already requested stop.
            //
            // A withheld ping is logged with its reason. Without that, a
            // watchdog kill is indistinguishable from a scheduling stall on a
            // loaded host, and the journal shows only systemd's own line —
            // which is what made 29 kills in a day unexplainable.
            if (watchdog_usec > 0) {
                const bool latched = static_cast<bool>(supervisor.failure());
                const bool alive = supervisor.all_alive();
                if (!latched && alive) {
                    alttech::notify_watchdog();
                } else {
                    std::string stalled;
                    for (const auto& [module, overdue_ms] : supervisor.stalled_modules()) {
                        if (!stalled.empty()) stalled += ",";
                        stalled += module + "(+" + std::to_string(overdue_ms / 1000) + "s)";
                    }
                    alttech::log::error("watchdog_ping_withheld",
                                        {{"latched_failure", latched},
                                         {"all_modules_alive", alive},
                                         {"stalled_modules", stalled},
                                         {"reason", latched ? "a module latched a fatal failure"
                                                            : "a module missed its liveness deadline"}});
                }
            }
        }
    }

    alttech::notify_stopping();
    shutdown.request_stop();
    for (auto& module : modules) {
        if (module.joinable()) module.join();
    }
    (**messaging).drain(3000);

    const auto failure = supervisor.failure();
    if (failure) {
        alttech::log::critical("analytics_service_stopped_after_failure",
                           {{"module", failure->module}, {"message", failure->error.message}});
        return 1;
    }
    alttech::log::info("analytics_service_stopped", {});
    return started ? 0 : 1;
}
