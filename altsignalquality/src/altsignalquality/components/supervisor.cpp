#include "altsignalquality/components/supervisor.hpp"

#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/readiness.hpp"
#include "altsignalquality/core/shutdown.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <thread>

namespace altsignalquality {

void Supervisor::add(std::unique_ptr<Component> component) {
    components_.push_back(std::move(component));
}

Result<void> Supervisor::init_all() {
    for (auto& c : components_) {
        log::set_thread_component(std::string(c->name()));
        if (auto r = c->init(); !r) {
            log::critical("component_init_failed", {{"component", std::string(c->name())},
                                                    {"message", r.error().message}});
            log::set_thread_component("supervisor");
            return r;
        }
    }
    log::set_thread_component("supervisor");
    return {};
}

int Supervisor::run_all() {
    auto& shutdown = ShutdownToken::instance();
    std::vector<std::jthread> threads;
    threads.reserve(components_.size());
    for (auto& c : components_) {
        threads.emplace_back([this, &c] {
            log::set_thread_component(std::string(c->name()));
            try {
                c->run();
            } catch (const std::exception& e) {
                failed_.store(true);
                log::critical("component_failed", {{"message", e.what()}});
            } catch (...) {
                failed_.store(true);
                log::critical("component_failed", {{"message", "unknown exception"}});
            }
            if (!ShutdownToken::instance().stop_requested()) {
                // run() may only return because shutdown was requested; an
                // early return is a failure even without an exception.
                failed_.store(true);
                log::critical("component_exited_early", {});
            }
            ShutdownToken::instance().request_stop(); // one down -> all down
        });
    }
    notify_ready(); // all inits already passed
    // systemd watchdog: the supervisor thread is the liveness anchor. Pings at
    // half the WatchdogSec budget; if this loop wedges, systemd kills and
    // restarts the whole service (Restart=on-failure).
    const int64_t watchdog_ms = watchdog_interval_ms();
    auto next_ping = std::chrono::steady_clock::now();
    while (!shutdown.stop_requested()) {
        if (watchdog_ms > 0 && std::chrono::steady_clock::now() >= next_ping) {
            notify_watchdog();
            next_ping = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(watchdog_ms);
        }
        shutdown.wait_for(std::chrono::milliseconds(500));
    }
    notify_stopping();
    threads.clear(); // join all component threads
    return failed_.load() ? 1 : 0;
}

} // namespace altsignalquality
