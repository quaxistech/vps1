#pragma once

#include "altml/core/config.hpp"

#include <string>

namespace altml::messaging {

// Idempotent JetStream provisioning for the project's declared topology.
//
// Owns (creates or compatibly updates) the output stream, the control stream
// and the inference durable. Requires, but never creates or updates, the
// input stream — it stays owned by the external feature producer. Retries the
// initial connect with a bounded, capped backoff so a cold boot does not race
// the NATS server.
//
// Runs inline at `altml run` startup (before any component touches NATS, which
// preserves the ordering the old oneshot unit provided) and behind the manual
// `altml nats-setup` subcommand. Returns false on any critical failure.
// Explicit maintenance flag only: daemon stopped, widen the known minute-only
// input filters in place without deleting/recreating its durable or cursor.
bool nats_provision(const std::string& url, const config::ProjectConfig& project,
                    bool widen_input_windows_stopped = false);

} // namespace altml::messaging
