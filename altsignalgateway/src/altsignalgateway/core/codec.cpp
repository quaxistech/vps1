#include "altsignalgateway/core/codec.hpp"

#include "altsignalgateway/core/hash.hpp"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <optional>

namespace altsignalgateway::messaging {

Result<altsignalquality::v1::Envelope> parse_upstream_envelope(std::string_view bytes) {
    altsignalquality::v1::Envelope env;
    if (!env.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        // ErrorCode::parse_error means the bytes are not this contract at all.
        return fail(ErrorCode::parse_error, "not a decodable altsignalquality.v1 envelope");
    }
    if (env.schema_version() != kUpstreamSchemaVersion) {
        // ErrorCode::validation_error means the bytes decoded but declare a
        // version this build does not speak. The caller maps the two onto
        // different rejection codes; conflating them would send an investigator
        // looking for a routing bug when the answer is an upgrade.
        return fail(ErrorCode::validation_error,
                    "upstream schema version " + std::to_string(env.schema_version()) +
                        "; this build decodes " + std::to_string(kUpstreamSchemaVersion));
    }
    return env;
}

Result<std::string> canonical_serialize(const google::protobuf::MessageLite& msg) {
    std::string out;
    {
        google::protobuf::io::StringOutputStream sink(&out);
        google::protobuf::io::CodedOutputStream coded(&sink);
        coded.SetSerializationDeterministic(true);
        if (!msg.SerializeToCodedStream(&coded)) {
            return fail(ErrorCode::internal_error, "canonical serialization failed");
        }
    }
    return out;
}

Result<std::string> canonical_hash(const google::protobuf::MessageLite& msg) {
    auto bytes = canonical_serialize(msg);
    if (!bytes) return std::unexpected(bytes.error());
    return sha256_hex(*bytes);
}

namespace {
// A bounded, zero-copy protobuf walk. Only the singular message paths that
// carry payloads are traversed; payload bytes themselves remain opaque.
struct WireField { uint32_t number; uint32_t type; std::string_view bytes; };
bool varint(std::string_view& in, uint64_t& value) {
    value = 0;
    for (unsigned i = 0; i < 10 && !in.empty(); ++i) {
        const auto c = static_cast<uint8_t>(in.front()); in.remove_prefix(1);
        if (i == 9 && c > 1) return false;
        value |= uint64_t(c & 127) << (7 * i);
        if ((c & 128) == 0) return true;
    }
    return false;
}
bool next_field(std::string_view& in, WireField& field) {
    uint64_t tag = 0, size = 0;
    if (!varint(in, tag) || tag >> 3 == 0 || tag >> 3 > 0x1fffffff) return false;
    field.number = static_cast<uint32_t>(tag >> 3);
    field.type = static_cast<uint32_t>(tag & 7);
    switch (field.type) {
        case 0: return varint(in, size);
        case 1: size = 8; break;
        case 2: if (!varint(in, size)) return false; break;
        case 5: size = 4; break;
        default: return false; // groups are not part of this contract
    }
    if (size > in.size()) return false;
    field.bytes = in.substr(0, static_cast<size_t>(size));
    in.remove_prefix(static_cast<size_t>(size));
    return true;
}
Result<std::optional<std::pair<uint32_t, std::string_view>>> singular(
    std::string_view in, uint32_t first, uint32_t last) {
    std::optional<std::pair<uint32_t, std::string_view>> result;
    while (!in.empty()) {
        WireField f{};
        if (!next_field(in, f)) return fail(ErrorCode::parse_error, "malformed wire field");
        if (f.number >= first && f.number <= last) {
            if (result || f.type != 2) return fail(ErrorCode::parse_error, "ambiguous wire message");
            result = std::pair{f.number, f.bytes};
        }
    }
    return result;
}
Result<size_t> verify_signal(std::string_view bytes) {
    auto envelope = singular(bytes, 1, 1);
    auto payload = singular(bytes, 2, 2);
    if (!envelope) return std::unexpected(envelope.error());
    if (!payload) return std::unexpected(payload.error());
    if (!*envelope || !*payload) return fail(ErrorCode::parse_error, "missing signal fields");
    auto hash = singular((**envelope).second, 11, 11);
    if (!hash) return std::unexpected(hash.error());
    if (!*hash || sha256_hex((**payload).second) != (**hash).second)
        return fail(ErrorCode::validation_error, "received payload hash mismatch");
    return 1;
}
} // namespace

Result<size_t> verify_server_payloads(std::string_view bytes) {
    if (bytes.size() > 16 * 1024 * 1024)
        return fail(ErrorCode::parse_error, "server frame exceeds client bound");
    auto body = singular(bytes, 1, 10);
    if (!body) return std::unexpected(body.error());
    if (!*body) return fail(ErrorCode::parse_error, "missing frame body");
    const auto [tag, content] = **body;
    if (tag == 4) return verify_signal(content);
    if (tag != 10) return 0;
    auto query = singular(content, 2, 5);
    if (!query) return std::unexpected(query.error());
    if (!*query) return fail(ErrorCode::parse_error, "missing query result");
    if ((**query).first != 2 && (**query).first != 4) return 0;
    auto signal = singular((**query).second, 1, 1);
    if (!signal) return std::unexpected(signal.error());
    if (!*signal) return fail(ErrorCode::parse_error, "missing query signal");
    return verify_signal((**signal).second);
}

Result<std::string> encode_server_frame(const altsignalgateway::v1::ServerFrame& frame) {
    // Server frames are serialized deterministically too. Not for the hash —
    // the payload carries its own — but so a captured frame is byte-reproducible
    // from the same inputs when a delivery dispute has to be settled.
    return canonical_serialize(frame);
}

namespace {

// Field numbers frozen by gateway.proto and asserted by
// tests/unit/test_wire_contract.cpp.
constexpr uint32_t kSignalMessageEnvelopeField = 1;
constexpr uint32_t kSignalMessagePayloadField = 2;
constexpr uint32_t kServerFrameSignalField = 4;
constexpr uint32_t kWireTypeLengthDelimited = 2;

void write_length_delimited(google::protobuf::io::CodedOutputStream* out, uint32_t field,
                            std::string_view bytes) {
    out->WriteTag((field << 3) | kWireTypeLengthDelimited);
    out->WriteVarint32(static_cast<uint32_t>(bytes.size()));
    out->WriteRaw(bytes.data(), static_cast<int>(bytes.size()));
}

} // namespace

Result<std::string> encode_signal_frame(const altsignalgateway::v1::SignalEnvelope& envelope,
                                        std::string_view canonical_payload) {
    auto env_bytes = canonical_serialize(envelope);
    if (!env_bytes) return std::unexpected(env_bytes.error());

    std::string message;
    {
        google::protobuf::io::StringOutputStream sink(&message);
        google::protobuf::io::CodedOutputStream coded(&sink);
        write_length_delimited(&coded, kSignalMessageEnvelopeField, *env_bytes);
        write_length_delimited(&coded, kSignalMessagePayloadField, canonical_payload);
    }
    std::string frame;
    {
        google::protobuf::io::StringOutputStream sink(&frame);
        google::protobuf::io::CodedOutputStream coded(&sink);
        write_length_delimited(&coded, kServerFrameSignalField, message);
    }
    return frame;
}

Result<altsignalgateway::v1::ClientFrame> decode_client_frame(std::string_view bytes) {
    altsignalgateway::v1::ClientFrame frame;
    if (!frame.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return fail(ErrorCode::parse_error, "malformed client frame");
    }
    return frame;
}

} // namespace altsignalgateway::messaging
