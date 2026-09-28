#pragma once

#include "altsignalgateway/core/error.hpp"
#include "altsignalgateway/domain/enrichment.hpp"
#include "gateway.pb.h"
#include "vendor/signalquality.pb.h"

#include <string>
#include <string_view>

namespace altsignalgateway::domain {

// Build the canonical FINAL TradeSignal payload.
//
// This is a pure projection. Every numeric field is either copied from
// `sig`, copied from `ctx`/`inst`, or is transport metadata from `meta`.
// Nothing is derived from market data, recomputed, rounded or defaulted.
//
// The single rule that governs every optional field: an upstream that encodes
// "absent" as 0.0 must not become a downstream that reads 0.0 as a measurement.
// Wherever that applies, the payload carries an explicit presence flag and the
// value is left at its protobuf default when the flag is false.
//
// `source_envelope` is the exact NATS payload and is embedded verbatim, so the
// canonical projection can always be checked against the producer's own
// message without asking VPS1 for it.
[[nodiscard]] altsignalgateway::v1::SignalPayload build_payload(
    const altsignalquality::v1::FinalTradeSignal& sig, std::string_view source_envelope,
    const ContextSnapshot& ctx, const InstrumentResolution& inst, const MacroSnapshot& macro,
    const IngestMetadata& meta);

// The macro block alone, for the extended-context query path (a journalled
// schema-1 payload has none; it is read as of T0 and projected the same way).
void fill_macro(altsignalgateway::v1::MacroSnapshot* out, const MacroSnapshot& macro);

// The history-page projection of one journalled payload: the decision-relevant
// scalars, copied, never recomputed. `delivery_state` and `last_ack_at_ms` are
// the journal row's, which the payload does not carry.
[[nodiscard]] altsignalgateway::v1::SignalSummary summarize_payload(
    const altsignalgateway::v1::SignalPayload& payload, std::string_view delivery_state,
    int64_t last_ack_at_ms);

// The journal facts one transmission is built from. Taken from the journal row
// rather than from a parsed payload: the row is the record of what was
// promised, and the envelope is a statement about that promise.
struct EnvelopeSource {
    // The schema the JOURNALLED payload has, not the one this build emits: a
    // schema-1 row replayed by a schema-2 server is still a schema-1 payload,
    // and saying otherwise would make its hash unverifiable against the wrong
    // shape.
    uint32_t schema_version = 0;
    std::string message_id;
    std::string signal_id;
    std::string payload_sha256;
    int64_t sequence = 0;
    int64_t created_at_ms = 0;
    int64_t expires_at_ms = 0;
    int64_t journaled_at_ms = 0;
    std::string producer_version;
};

// Build the transport envelope for one TRANSMISSION of a journalled signal.
//
// `payload_sha256` is passed through from the journal rather than recomputed,
// so the value that was stored and the value that is sent are provably the same
// one. `published_at_ms` is the instant of THIS transmission: the same payload
// can legitimately go out more than once and each time has its own.
[[nodiscard]] altsignalgateway::v1::SignalEnvelope build_envelope(const EnvelopeSource& src,
                                                                  bool replay,
                                                                  uint32_t delivery_attempt,
                                                                  int64_t published_at_ms);

// Map AltStrategy's persisted ContextQuality token onto the wire enum.
// Unknown tokens map to UNSPECIFIED rather than to a plausible neighbour.
[[nodiscard]] altstrategy::v1::ContextQuality parse_context_quality(std::string_view token);

} // namespace altsignalgateway::domain
