#pragma once

#include "altsignalgateway/core/error.hpp"
#include "gateway.pb.h"
#include "vendor/signalquality.pb.h"

#include <string>
#include <string_view>

namespace altsignalgateway::messaging {

// The schema version of the upstream contract this build decodes.
inline constexpr uint32_t kUpstreamSchemaVersion = 1;

// Parse the inbound altsignalquality.v1 envelope.
//
// A message that is not a FinalTradeSignal is not an error for this service —
// the subject filter is a contract, but a stream can legitimately carry other
// payloads — so absence is reported as `not_found` and the message is dropped
// with an ack rather than terminated as broken.
Result<altsignalquality::v1::Envelope> parse_upstream_envelope(std::string_view bytes);

// Canonical, deterministic serialization.
//
// Protobuf's default serializer makes no ordering promise for map fields and is
// free to reorder in principle; deterministic mode pins field order and map
// order. This contract additionally contains NO map fields at all (see the
// NamedValue comment in gateway.proto), so the canonical bytes are a pure
// function of the field values on any runtime, which is what makes the hash
// below reproducible by an external consumer.
Result<std::string> canonical_serialize(const google::protobuf::MessageLite& msg);

// sha256 over the canonical serialization. This is the value that appears in
// SignalEnvelope.payload_sha256 and in the journal.
Result<std::string> canonical_hash(const google::protobuf::MessageLite& msg);

// Verify exact received payload slices, including query nesting. No
// reserialization is allowed at this consumer boundary. History carries only
// summaries/envelopes, so there is no payload there to verify.
Result<size_t> verify_server_payloads(std::string_view bytes);

// Frame helpers for the WebSocket protocol.
Result<std::string> encode_server_frame(const altsignalgateway::v1::ServerFrame& frame);

// Build a ServerFrame carrying one SignalMessage WITHOUT re-serializing the
// payload.
//
// The payload's bytes are the ones the journal stored and hashed. Parsing them
// into a SignalPayload and letting protobuf write them out again would produce
// a DIFFERENT byte string whenever the running build's schema differs from the
// one that wrote the row — protobuf preserves unknown fields but re-emits them
// after the known ones. A rollback to an older build would then transmit bytes
// that do not match the hash travelling in the envelope beside them, and the
// integrity check would fail on a signal that is perfectly intact.
//
// Splicing the stored bytes in as the length-delimited `payload` field removes
// that whole class of problem: what is hashed, what is stored and what is sent
// are one byte string.
Result<std::string> encode_signal_frame(const altsignalgateway::v1::SignalEnvelope& envelope,
                                        std::string_view canonical_payload);
Result<altsignalgateway::v1::ClientFrame> decode_client_frame(std::string_view bytes);

} // namespace altsignalgateway::messaging
