#pragma once

#include "altmacro/core/config.hpp"

#include <string>

namespace altmacro::messaging {

// Idempotent JetStream provisioning for altmacro's output stream.
//
// altmacro owns exactly one stream and creates no consumers: downstream
// services declare their own durables, so this service cannot accidentally
// reset a consumer's cursor by redeploying.
//
// The stream is a delivery buffer, not an archive. PostgreSQL holds the
// history and is the system of record; a consumer that falls further behind
// than the stream's retention backfills from the database rather than from
// here. That is why the retention below is measured in weeks and not years.
//
// Returns false on any failure so that startup fails loudly rather than
// running half-wired.
bool nats_provision(const std::string& url, const NatsConfig& cfg);

} // namespace altmacro::messaging
