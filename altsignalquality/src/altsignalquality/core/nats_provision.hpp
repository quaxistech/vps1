#pragma once

#include "altsignalquality/core/config.hpp"

#include <string>

namespace altsignalquality::messaging {

// Idempotent JetStream provisioning for the AltSignalQuality topology.
//
// Owns: the output stream (final signals + evaluations + model events) and the
// durable consumer this service reads AltStrategy's candidates through.
// Requires, but never creates or updates: AltStrategy's decision stream.
// Returns false on any failure so startup can fail loudly instead of running
// half-wired.
bool nats_provision(const std::string& url, const config::WiringConfig& wiring);

} // namespace altsignalquality::messaging
