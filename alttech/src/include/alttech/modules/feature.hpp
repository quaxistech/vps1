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

// Runs the feature module until the process-wide shutdown token is requested.
// PostgreSQL, NATS and process lifecycle are owned by the application layer.
alttech::Result<void> run_feature(
    const YAML::Node& config,
    alttech::persistence::PgPool& pool,
    alttech::messaging::NatsClient& nats,
    alttech::application::ModuleSupervisor& supervisor);

} // namespace alttech::modules
