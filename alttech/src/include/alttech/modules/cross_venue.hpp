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

// Runs the fifth supervised module: event-time alignment plus cross-venue
// consensus, divergence, lead-lag, funding/OI normalization, liquidity ranking,
// consensus structure, Bitget projections and the global regime, until the
// process-wide shutdown token is requested. A transient shortage of one
// reference venue degrades the module (DEGRADED) rather than stopping the
// process; only a schema/contract integrity breach is fatal.
// PostgreSQL, NATS and process lifecycle are owned by the application layer.
alttech::Result<void> run_cross_venue(
    const YAML::Node& config,
    alttech::persistence::PgPool& pool,
    alttech::messaging::NatsClient& nats,
    alttech::application::ModuleSupervisor& supervisor);

} // namespace alttech::modules
