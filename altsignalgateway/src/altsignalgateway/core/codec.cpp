#include "altsignalgateway/core/codec.hpp"

#include "altsignalgateway/core/hash.hpp"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

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
