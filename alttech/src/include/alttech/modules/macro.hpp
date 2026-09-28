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

// Runs the sixth supervised module: the macro / cross-asset block.
//
// It reads AltMacro's raw external series through the read-only altmacro_ref
// foreign schema, derives a compact set of measured quantities -- returns,
// realized volatility, spreads between the three CNY observations, rate curve
// constructions -- and publishes them on the 1m grid as a GLOBAL-scope feature
// set that belongs to no venue and no pair.
//
// It states nothing about direction. There is no rule here that says a weaker
// renminbi means anything for crypto; whether any of these columns predicts
// anything is for the model to establish.
//
// A gap in the external data degrades the block (validity bits clear) rather
// than stopping the process, exactly as a missing reference venue does for
// cross_venue.
alttech::Result<void> run_macro(
    const YAML::Node& config,
    alttech::persistence::PgPool& pool,
    alttech::messaging::NatsClient& nats,
    alttech::application::ModuleSupervisor& supervisor);

} // namespace alttech::modules
