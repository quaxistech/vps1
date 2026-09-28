#pragma once

#include "altsignalgateway/core/config.hpp"
#include "altsignalgateway/core/error.hpp"

#include <string>

namespace altsignalgateway::components {

// Owns the whole running service: the journal, the ingest loop, the delivery
// pump and the TLS WebSocket listener.
//
// Startup order is not arbitrary. The journal comes first (nothing else can be
// promised without it), then recovery of whatever the last process left
// in-flight, then the listener, then ingest. A client that connects before
// ingest has started sees an empty backlog and waits, which is correct; the
// reverse order would journal signals with no delivery path attached and defer
// their first transmission to the next pump tick.
//
// SIGTERM runs the same sequence backwards, and nothing about it depends on a
// client being present: VPS1 finishes what it is doing whether or not anyone is
// listening.
Result<int> run_service(const config::RuntimeConfig& cfg, const std::string& version);

} // namespace altsignalgateway::components
