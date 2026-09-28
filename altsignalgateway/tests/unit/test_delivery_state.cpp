// The delivery state machine.
//
// This table is duplicated in PostgreSQL (a trigger in
// db/migrations/0001_altsignalgateway_schema.sql). tests/shell/
// test_schema_invariants.sh asserts the two agree; this file asserts the C++
// half is right in the first place.

#include "altsignalgateway/domain/delivery_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace altsignalgateway::domain;

namespace {
constexpr std::array<DeliveryState, 6> kAll{DeliveryState::journaled, DeliveryState::queued,
                                            DeliveryState::sent,      DeliveryState::acked,
                                            DeliveryState::expired,   DeliveryState::failed};
}

TEST_CASE("the happy path is the only path to ACKED") {
    CHECK(is_legal_transition(DeliveryState::journaled, DeliveryState::queued));
    CHECK(is_legal_transition(DeliveryState::queued, DeliveryState::sent));
    CHECK(is_legal_transition(DeliveryState::sent, DeliveryState::acked));

    // A signal cannot be acknowledged without having been sent, and cannot be
    // sent without having been queued. Anything else would let a bug record a
    // delivery that never left the process.
    CHECK_FALSE(is_legal_transition(DeliveryState::journaled, DeliveryState::sent));
    CHECK_FALSE(is_legal_transition(DeliveryState::journaled, DeliveryState::acked));
    CHECK_FALSE(is_legal_transition(DeliveryState::queued, DeliveryState::acked));
}

TEST_CASE("a lost session returns in-flight work to the journal") {
    CHECK(is_legal_transition(DeliveryState::queued, DeliveryState::journaled));
    CHECK(is_legal_transition(DeliveryState::sent, DeliveryState::journaled));
    // But an acknowledged signal is never redelivered by a requeue: it is done.
    CHECK_FALSE(is_legal_transition(DeliveryState::acked, DeliveryState::journaled));
}

TEST_CASE("terminal states are terminal") {
    for (const auto to : kAll) {
        if (to == DeliveryState::acked) continue;  // ACKED -> ACKED is the idempotent no-op
        CHECK_FALSE(is_legal_transition(DeliveryState::acked, to));
        CHECK_FALSE(is_legal_transition(DeliveryState::expired, to));
        CHECK_FALSE(is_legal_transition(DeliveryState::failed, to));
    }
    CHECK_FALSE(is_legal_transition(DeliveryState::expired, DeliveryState::acked));
    CHECK_FALSE(is_legal_transition(DeliveryState::failed, DeliveryState::acked));
    CHECK(is_terminal(DeliveryState::acked));
    CHECK(is_terminal(DeliveryState::expired));
    CHECK(is_terminal(DeliveryState::failed));
}

TEST_CASE("a duplicate ACK is legal") {
    // At-least-once delivery means a client can legitimately acknowledge the
    // same message twice — its own ACK was lost, or it reconnected before the
    // cursor advanced. Treating the second as an illegal transition would drop
    // a connection over correct client behaviour.
    CHECK(is_legal_transition(DeliveryState::acked, DeliveryState::acked));
    // No other self-transition is a state CHANGE, so none is asserted legal
    // here; the database trigger allows the non-terminal ones because an
    // UPDATE that only touches delivery_attempts keeps the state the same.
}

TEST_CASE("expiry and failure are reachable from every in-flight state") {
    for (const auto from : {DeliveryState::journaled, DeliveryState::queued, DeliveryState::sent}) {
        CHECK(is_legal_transition(from, DeliveryState::expired));
        CHECK(is_legal_transition(from, DeliveryState::failed));
        CHECK(is_deliverable(from));
        CHECK_FALSE(is_terminal(from));
    }
}

TEST_CASE("state names round-trip") {
    for (const auto s : kAll) {
        const auto parsed = parse_delivery_state(delivery_state_name(s));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == s);
    }
    // An unknown token is rejected, never mapped onto a plausible neighbour.
    CHECK_FALSE(parse_delivery_state("DELIVERED").has_value());
    CHECK_FALSE(parse_delivery_state("").has_value());
    CHECK_FALSE(parse_delivery_state("acked").has_value());
}
