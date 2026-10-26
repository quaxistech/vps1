// Regression: a delivered signal carries the EXACT bytes the journal hashed.
//
// THE DEFECT THIS FILE EXISTS FOR. The first implementation verified a payload
// by parsing it and re-serializing it, then built the outbound frame from the
// parsed message. Protobuf preserves unknown fields but re-emits them AFTER the
// known ones, so a build whose schema differs from the one that wrote the row —
// which is exactly what a rollback is — produced different bytes, failed its own
// hash check, and marked the signal FAILED. FAILED is terminal, so a rollback
// would have permanently destroyed the deliverability of every signal written
// by the newer build, while OPERATIONS.md promised that a rollback was safe.
//
// The fix: hash the stored bytes, and splice them into the frame verbatim.

#include "altsignalgateway/core/codec.hpp"
#include "altsignalgateway/core/hash.hpp"
#include "gateway.pb.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace pb = altsignalgateway::v1;
using namespace altsignalgateway;

namespace {

pb::SignalEnvelope envelope_fixture() {
    pb::SignalEnvelope e;
    e.set_schema_version(1);
    e.set_protocol_version(1);
    e.set_message_id("asgw_00112233445566778899aabbccddeeff");
    e.set_signal_id("sqfin_deadbeef");
    e.set_sequence(4242);
    e.set_signal_type("FINAL_TRADE_SIGNAL");
    e.set_created_at_ms(1'787'000'000'000);
    e.set_expires_at_ms(1'787'000'900'000);
    e.set_journaled_at_ms(1'787'000'000'100);
    e.set_published_at_ms(1'787'000'000'200);
    e.set_payload_sha256(std::string(64, 'a'));
    e.set_producer_service("altsignalgateway");
    e.set_producer_version("1.0.0");
    return e;
}

pb::SignalPayload payload_fixture() {
    pb::SignalPayload p;
    auto* t = p.mutable_trade_plan();
    t->set_exchange("bitget");
    t->set_symbol("APTUSDT");
    t->set_canonical_pair_id(15);
    t->set_entry_price(0.5907);
    t->set_stop_loss_price(0.59514);
    t->set_take_profit_price(0.58239);
    p.mutable_provenance()->set_signal_proposal_id("sig_abc");
    p.set_source_envelope(std::string("\x00\x01\x02raw upstream bytes\xff", 22));
    return p;
}

} // namespace

TEST_CASE("the spliced frame parses as a ServerFrame carrying the signal") {
    const auto env = envelope_fixture();
    auto stored = messaging::canonical_serialize(payload_fixture());
    REQUIRE(stored.has_value());

    auto frame_bytes = messaging::encode_signal_frame(env, *stored);
    REQUIRE(frame_bytes.has_value());

    pb::ServerFrame frame;
    REQUIRE(frame.ParseFromString(*frame_bytes));
    REQUIRE(frame.body_case() == pb::ServerFrame::kSignal);
    REQUIRE(frame.signal().has_envelope());
    REQUIRE(frame.signal().has_payload());
    CHECK(frame.signal().envelope().sequence() == 4242);
    CHECK(frame.signal().envelope().signal_id() == "sqfin_deadbeef");
    CHECK(frame.signal().payload().trade_plan().symbol() == "APTUSDT");
    CHECK(frame.signal().payload().trade_plan().entry_price() == 0.5907);
}

TEST_CASE("the payload on the wire is byte-identical to the stored payload") {
    const auto env = envelope_fixture();
    auto stored = messaging::canonical_serialize(payload_fixture());
    REQUIRE(stored.has_value());
    auto frame_bytes = messaging::encode_signal_frame(env, *stored);
    REQUIRE(frame_bytes.has_value());

    // The whole point: what a consumer hashes is what the journal hashed.
    pb::ServerFrame frame;
    REQUIRE(frame.ParseFromString(*frame_bytes));
    auto round_trip = messaging::canonical_serialize(frame.signal().payload());
    REQUIRE(round_trip.has_value());
    CHECK(*round_trip == *stored);
    CHECK(sha256_hex(*round_trip) == sha256_hex(*stored));

    // And the stored bytes appear in the frame literally, not merely
    // equivalently.
    CHECK(frame_bytes->find(*stored) != std::string::npos);
}

TEST_CASE("an opaque payload survives the splice untouched") {
    // Stand-in for a payload written by a build whose schema this one does not
    // know: field 999, which SignalPayload does not declare. Parsing and
    // re-serializing would move it; splicing cannot.
    std::string opaque;
    {
        // field 999, wire type 2 (length-delimited), 4 bytes of content
        const uint32_t tag = (999u << 3) | 2u;
        opaque.push_back(static_cast<char>((tag & 0x7F) | 0x80));
        opaque.push_back(static_cast<char>(((tag >> 7) & 0x7F) | 0x80));
        opaque.push_back(static_cast<char>((tag >> 14) & 0x7F));
        opaque.push_back(4);
        opaque += "abcd";
    }
    auto known = messaging::canonical_serialize(payload_fixture());
    REQUIRE(known.has_value());
    // The unknown field FIRST, which is the ordering a re-serialization would
    // destroy by moving it to the end.
    const std::string stored = opaque + *known;

    auto frame_bytes = messaging::encode_signal_frame(envelope_fixture(), stored);
    REQUIRE(frame_bytes.has_value());
    CHECK(frame_bytes->find(stored) != std::string::npos);

    // It still parses, and the known fields still read correctly.
    pb::ServerFrame frame;
    REQUIRE(frame.ParseFromString(*frame_bytes));
    CHECK(frame.signal().payload().trade_plan().symbol() == "APTUSDT");

    // And here is the defect, demonstrated: re-serializing the parsed message
    // does NOT reproduce the stored bytes. Under the old implementation this
    // was a hash mismatch and a permanent FAILED.
    auto reserialized = messaging::canonical_serialize(frame.signal().payload());
    REQUIRE(reserialized.has_value());
    CHECK(*reserialized != stored);
    CHECK(sha256_hex(*reserialized) != sha256_hex(stored));
}

TEST_CASE("an empty payload is still a well-formed frame") {
    auto frame_bytes = messaging::encode_signal_frame(envelope_fixture(), std::string());
    REQUIRE(frame_bytes.has_value());
    pb::ServerFrame frame;
    REQUIRE(frame.ParseFromString(*frame_bytes));
    CHECK(frame.signal().envelope().sequence() == 4242);
}

TEST_CASE("a large payload is framed with a correct multi-byte length") {
    // Crosses the one-byte and two-byte varint boundaries, which is where a
    // hand-written length prefix goes wrong if it goes wrong at all.
    for (const size_t n : {127u, 128u, 16383u, 16384u, 200000u}) {
        pb::SignalPayload big = payload_fixture();
        big.set_source_envelope(std::string(n, 'x'));
        auto stored = messaging::canonical_serialize(big);
        REQUIRE(stored.has_value());
        auto frame_bytes = messaging::encode_signal_frame(envelope_fixture(), *stored);
        REQUIRE(frame_bytes.has_value());
        pb::ServerFrame frame;
        INFO("payload bytes: " << n);
        REQUIRE(frame.ParseFromString(*frame_bytes));
        CHECK(frame.signal().payload().source_envelope().size() == n);
        auto round_trip = messaging::canonical_serialize(frame.signal().payload());
        REQUIRE(round_trip.has_value());
        CHECK(*round_trip == *stored);
    }
}

namespace {
std::string wire_message(unsigned tag, std::string_view content) {
    std::string result;
    const auto put = [&](uint64_t value) {
        do {
            const auto byte = static_cast<unsigned char>(value & 127);
            value >>= 7;
            result.push_back(static_cast<char>(byte | (value ? 128 : 0)));
        } while (value);
    };
    put((tag << 3) | 2); put(content.size()); result += content;
    return result;
}
}

TEST_CASE("consumer verifies opaque wire bytes in delivery and query responses") {
    const auto known = *messaging::canonical_serialize(payload_fixture());
    const auto unknown = wire_message(999, "future schema");
    for (const auto& payload : {unknown + known, known + unknown}) {
        auto env = envelope_fixture();
        env.set_payload_sha256(sha256_hex(payload));
        const auto envelope = *messaging::canonical_serialize(env);
        // Reversed known field order is legal too.
        const auto signal = wire_message(2, payload) + wire_message(1, envelope);
        const auto delivered = wire_message(4, signal);
        REQUIRE(messaging::verify_server_payloads(delivered));
        CHECK(*messaging::verify_server_payloads(delivered) == 1);
        for (unsigned result_tag : {2u, 4u}) {
            const auto query = wire_message(10, wire_message(1, "request") +
                                               wire_message(result_tag, wire_message(1, signal)));
            REQUIRE(messaging::verify_server_payloads(query));
            CHECK(*messaging::verify_server_payloads(query) == 1);
        }
        CHECK_FALSE(messaging::verify_server_payloads(
            wire_message(4, signal + wire_message(2, payload))));
        CHECK_FALSE(messaging::verify_server_payloads(delivered + delivered));
        CHECK_FALSE(messaging::verify_server_payloads(wire_message(4,
            wire_message(1, envelope) + wire_message(2, payload + "x"))));
        CHECK_FALSE(messaging::verify_server_payloads(delivered.substr(0, delivered.size() - 1)));
    }
    // History pages contain summaries/envelopes, not SignalPayload bytes.
    const auto history = wire_message(10, wire_message(3, wire_message(7, "")));
    REQUIRE(messaging::verify_server_payloads(history));
    CHECK(*messaging::verify_server_payloads(history) == 0);
    CHECK_FALSE(messaging::verify_server_payloads(std::string(10, '\xff')));
    CHECK_FALSE(messaging::verify_server_payloads(std::string(17 * 1024 * 1024, 'x')));
}
