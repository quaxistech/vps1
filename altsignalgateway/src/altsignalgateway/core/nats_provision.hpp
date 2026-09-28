#pragma once

#include "altsignalgateway/core/config.hpp"

#include <string>

namespace altsignalgateway::messaging {

// Idempotent JetStream provisioning for the AltSignalGateway-owned topology.
//
// This service owns EXACTLY ONE JetStream resource: its durable consumer on
// AltSignalQuality's output stream. It creates no stream, publishes to no
// subject and reconfigures nothing that belongs to another service —
// AltSignalQuality's own integration document prescribes precisely this shape
// for a downstream consumer, and a consumer that retunes a producer's stream is
// how retention budgets get silently reassigned.
//
// `deliver_all` selects the delivery policy applied WHEN THE DURABLE IS FIRST
// CREATED (it is ignored afterwards; a restart always resumes at the stored
// cursor). See docs/NATS.md for why `new` is the default.
bool nats_provision(const std::string& url, const config::WiringConfig& wiring, bool deliver_all);

} // namespace altsignalgateway::messaging
