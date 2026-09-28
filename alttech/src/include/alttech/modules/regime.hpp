#pragma once

#include "alttech/common/error.hpp"

namespace YAML {
class Node;
}

namespace alttech::messaging {
class NatsClient;
}

namespace alttech::persistence {
class PgPool;
}

namespace alttech::application {
class ModuleSupervisor;
}

namespace alttech::modules {

// Runs deterministic regime classification until process-wide shutdown is
// requested. PostgreSQL, NATS and process lifecycle are application-owned.
alttech::Result<void> run_regime(
    const YAML::Node& config,
    alttech::persistence::PgPool& pool,
    alttech::messaging::NatsClient& nats,
    alttech::application::ModuleSupervisor& supervisor);

} // namespace alttech::modules
