// The protocol-2 query path, everything that can be decided without a
// database: what is refused before any SQL runs, and the cursor's round trip.
#include "altsignalgateway/components/query_service.hpp"

#include <catch2/catch_test_macros.hpp>

using altsignalgateway::components::decode_cursor;
using altsignalgateway::components::encode_cursor;
using altsignalgateway::components::validate_query;
namespace pb = altsignalgateway::v1;
namespace as = altstrategy::v1;

namespace {
altsignalgateway::config::QueryConfig cfg() {
    altsignalgateway::config::QueryConfig c;
    c.default_page_size = 50;
    c.max_page_size = 200;
    return c;
}
}

TEST_CASE("the history cursor is a total round trip and nothing else parses") {
    // A malformed token must not silently mean "page one": a consumer that
    // corrupted its cursor would then re-read the head and believe it had
    // continued.
    CHECK(decode_cursor(encode_cursor(370)) == 370);
    CHECK(decode_cursor(encode_cursor(1)) == 1);
    CHECK_FALSE(decode_cursor("").has_value());
    CHECK_FALSE(decode_cursor("370").has_value());
    CHECK_FALSE(decode_cursor("s:0").has_value());
    CHECK_FALSE(decode_cursor("s:-5").has_value());
    CHECK_FALSE(decode_cursor("s:12x").has_value());
    CHECK_FALSE(decode_cursor("s:").has_value());
}

TEST_CASE("a query names exactly what it wants, or it is refused") {
    pb::QueryRequest req;
    req.set_request_id("r1");
    // No body.
    REQUIRE(validate_query(req, cfg()).has_value());
    CHECK(validate_query(req, cfg())->code() == pb::ERR_INVALID_QUERY);

    // GetSignal: exactly one identity.
    req.mutable_get_signal();
    CHECK(validate_query(req, cfg()).has_value());
    req.mutable_get_signal()->set_signal_id("sqfin_e66ce125b5cce7f6292a6137cc51a01d");
    CHECK_FALSE(validate_query(req, cfg()).has_value());
    req.mutable_get_signal()->set_sequence(370);
    CHECK(validate_query(req, cfg()).has_value());
    req.mutable_get_signal()->clear_signal_id();
    CHECK_FALSE(validate_query(req, cfg()).has_value());
    // An identifier with SQL in it is a bad identifier, not a query.
    req.mutable_get_signal()->clear_sequence();
    req.mutable_get_signal()->set_signal_id("x' OR 1=1 --");
    CHECK(validate_query(req, cfg()).has_value());

    // Empty request_id: the caller could not correlate the answer.
    pb::QueryRequest anon;
    anon.mutable_get_signal()->set_sequence(1);
    CHECK(validate_query(anon, cfg()).has_value());
}

TEST_CASE("a history page is bounded before it is read, never clamped after") {
    pb::QueryRequest req;
    req.set_request_id("h");
    auto* h = req.mutable_get_history();
    CHECK_FALSE(validate_query(req, cfg()).has_value());   // defaults are fine
    h->set_page_size(200);
    CHECK_FALSE(validate_query(req, cfg()).has_value());
    h->set_page_size(201);
    REQUIRE(validate_query(req, cfg()).has_value());
    CHECK(validate_query(req, cfg())->code() == pb::ERR_INVALID_QUERY);
    h->set_page_size(0);
    h->set_cursor("garbage");
    CHECK(validate_query(req, cfg()).has_value());
    h->set_cursor(encode_cursor(370));
    CHECK_FALSE(validate_query(req, cfg()).has_value());

    // Filters: vocabulary is checked, ranges must be ordered.
    h->mutable_filter()->set_delivery_state("DELIVERED");
    CHECK(validate_query(req, cfg()).has_value());
    h->mutable_filter()->set_delivery_state("ACKED");
    CHECK_FALSE(validate_query(req, cfg()).has_value());
    h->mutable_filter()->set_side(as::SIDE_SHORT);
    CHECK_FALSE(validate_query(req, cfg()).has_value());
    h->mutable_filter()->set_from_created_at_ms(2000);
    h->mutable_filter()->set_to_created_at_ms(1000);
    CHECK(validate_query(req, cfg()).has_value());
    h->mutable_filter()->set_to_created_at_ms(3000);
    CHECK_FALSE(validate_query(req, cfg()).has_value());
    h->mutable_filter()->set_symbol("BTC USDT; DROP TABLE");
    CHECK(validate_query(req, cfg()).has_value());
    h->mutable_filter()->set_symbol("BTCUSDT");
    CHECK_FALSE(validate_query(req, cfg()).has_value());
}

TEST_CASE("a context request states its detail level") {
    pb::QueryRequest req;
    req.set_request_id("c");
    auto* c = req.mutable_get_context();
    c->set_signal_id("sqfin_abc");
    CHECK(validate_query(req, cfg()).has_value());          // UNSPECIFIED is refused
    c->set_detail(pb::CONTEXT_STANDARD);
    CHECK_FALSE(validate_query(req, cfg()).has_value());
    c->set_detail(pb::CONTEXT_FULL);
    CHECK_FALSE(validate_query(req, cfg()).has_value());
    c->clear_signal_id();
    CHECK(validate_query(req, cfg()).has_value());
}
