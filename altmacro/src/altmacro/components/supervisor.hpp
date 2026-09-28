#pragma once

#include "altmacro/components/poller.hpp"
#include "altmacro/components/publisher.hpp"
#include "altmacro/core/config.hpp"
#include "altmacro/core/error.hpp"
#include "altmacro/core/nats_client.hpp"
#include "altmacro/core/pg.hpp"
#include "altmacro/data/store.hpp"
#include "altmacro/providers/registry.hpp"

#include <memory>

namespace altmacro::components {

// Owns the running service: one process, one loop.
class Supervisor {
public:
    explicit Supervisor(Config cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] Result<void> run();

private:
    Config cfg_;
    std::unique_ptr<persistence::PgPool> pool_;
    std::unique_ptr<data::Store> store_;
    std::unique_ptr<messaging::NatsClient> nats_;
    std::unique_ptr<Publisher> publisher_;
    std::unique_ptr<providers::Registry> registry_;
};

} // namespace altmacro::components
