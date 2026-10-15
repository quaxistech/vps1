#pragma once

// Application: the altstrategy-service composition root. Owns the single shared
// PostgreSQL pool and NATS connection, holds a session advisory lock so only one
// instance acts on the altstrategy schema, and runs the event-driven decision
// engine + the internal maintenance scheduler (one binary, one process, one
// systemd unit — §2). No separate archive / outcome / export services exist.

#include "altstrategy/config/service_bootstrap.hpp"

#include <atomic>
#include <memory>

namespace pqxx { class connection; }
namespace altstrategy::persistence { class PgPool; }
namespace altstrategy::messaging { class NatsClient; class Subscription; }

namespace altstrategy::engine_io {
class DecisionStore;
class MarketReader;
class SnapshotExporter;
class OutboxPublisher;
class DecisionEngine;
class MaintenanceScheduler;
}

namespace altstrategy::app {

class Application {
public:
    explicit Application(config::ServiceContext ctx);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] bool setup();
    [[nodiscard]] int run();

private:
    [[nodiscard]] bool acquire_singleton_lock();

    config::ServiceContext ctx_;

    std::unique_ptr<persistence::PgPool> pool_;
    std::unique_ptr<messaging::NatsClient> nats_;
    std::unique_ptr<pqxx::connection> singleton_lock_;
    void verify_singleton_lock();

    std::unique_ptr<engine_io::DecisionStore> store_;
    std::unique_ptr<engine_io::MarketReader> reader_;
    std::unique_ptr<engine_io::SnapshotExporter> exporter_;
    // Declared after the store and NATS client it uses and before the engine
    // that notifies it, so it is stopped in between.
    std::unique_ptr<engine_io::OutboxPublisher> publisher_;
    std::unique_ptr<engine_io::DecisionEngine> engine_;
    std::unique_ptr<engine_io::MaintenanceScheduler> scheduler_;
    std::unique_ptr<messaging::Subscription> feedback_sub_;

    std::atomic<int> exit_code_{0};
};

} // namespace altstrategy::app
