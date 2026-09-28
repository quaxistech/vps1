#pragma once

#include "altmacro/core/config.hpp"
#include "altmacro/core/error.hpp"
#include "altmacro/core/nats_client.hpp"
#include "altmacro/data/store.hpp"
#include "altmacro/domain/series.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace altmacro::components {

// Publishes observations to JetStream.
//
// Every message carries its full provenance — provider, instrument, frequency,
// series class, denomination, timestamp semantics and quality — because a
// consumer that receives only a number and a time has no way to tell an
// offshore market quote from a central bank's fixing, and this service exists
// precisely so that it never has to guess.
class Publisher {
public:
    Publisher(const NatsConfig& cfg, messaging::NatsClient* nats) : cfg_(cfg), nats_(nats) {}

    // Subject-safe form of an instrument code: dots become underscores, because
    // a dot is NATS's token separator and FX.USDCNH would otherwise split into
    // two tokens and silently widen every wildcard subscription.
    [[nodiscard]] static std::string subject_token(const std::string& instrument_code);

    [[nodiscard]] std::string bar_subject(const std::string& instrument_code,
                                          domain::PriceBasis basis) const;
    [[nodiscard]] std::string reference_subject(const std::string& instrument_code) const;

    // `heartbeat` is consulted every few messages. Publishing is a synchronous
    // round trip per message, and a restart hands this function several hundred
    // at once: without a liveness signal inside the loop the service is silent
    // for minutes and its own watchdog kills it mid-publish. Returning true
    // aborts, which is how shutdown stays prompt.
    using Heartbeat = std::function<bool()>;

    [[nodiscard]] Result<int64_t>
    publish_bars(const data::SourceBinding& b, const std::string& timeframe,
                 const std::vector<domain::PriceBar>& bars, const Heartbeat& heartbeat = {});

    [[nodiscard]] Result<int64_t>
    publish_reference(const data::SourceBinding& b,
                      const std::vector<domain::ReferenceValue>& values,
                      const Heartbeat& heartbeat = {});

    // Deterministic message id, so a republish after a restart deduplicates at
    // the server instead of appending a second copy of the same observation.
    [[nodiscard]] static std::string message_id(const data::SourceBinding& b,
                                                const std::string& timeframe, int64_t event_time_ms,
                                                domain::PriceBasis basis);

    static constexpr const char* kSchemaVersion = "altmacro-series:1";

private:
    NatsConfig cfg_;
    messaging::NatsClient* nats_;
};

} // namespace altmacro::components
