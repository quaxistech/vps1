// The WebSocket session state machine.
//
// The table below is the whole of docs/WEBSOCKET_PROTOCOL.md's "what is legal
// where" section. It is asserted exhaustively — every state against every frame
// — because a boundary that accepts an unexpected frame is a boundary whose
// behaviour nobody can state.

#include "altsignalgateway/domain/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace altsignalgateway::domain;

namespace {
constexpr std::array<SessionState, 7> kStates{
    SessionState::connected, SessionState::hello_sent, SessionState::authenticated,
    SessionState::resuming,  SessionState::ready,      SessionState::closing,
    SessionState::closed};
constexpr std::array<ClientFrameKind, 7> kFrames{
    ClientFrameKind::hello, ClientFrameKind::auth,          ClientFrameKind::resume,
    ClientFrameKind::ack,   ClientFrameKind::heartbeat_ack, ClientFrameKind::goodbye,
    ClientFrameKind::query};
}

TEST_CASE("the handshake admits exactly one frame at a time") {
    CHECK(accepts(SessionState::connected, ClientFrameKind::hello));
    CHECK(accepts(SessionState::hello_sent, ClientFrameKind::auth));
    CHECK(accepts(SessionState::authenticated, ClientFrameKind::resume));

    // Skipping a step is a protocol violation, not something to tolerate.
    CHECK_FALSE(accepts(SessionState::connected, ClientFrameKind::auth));
    CHECK_FALSE(accepts(SessionState::connected, ClientFrameKind::resume));
    CHECK_FALSE(accepts(SessionState::connected, ClientFrameKind::ack));
    CHECK_FALSE(accepts(SessionState::hello_sent, ClientFrameKind::resume));
    CHECK_FALSE(accepts(SessionState::hello_sent, ClientFrameKind::ack));
    CHECK_FALSE(accepts(SessionState::authenticated, ClientFrameKind::ack));
}

TEST_CASE("a second HELLO or AUTH is refused") {
    // Re-authenticating on a live connection would let a captured AUTH frame be
    // replayed against a session whose nonce has already been consumed.
    for (const auto st : {SessionState::authenticated, SessionState::resuming,
                          SessionState::ready}) {
        CHECK_FALSE(accepts(st, ClientFrameKind::hello));
        CHECK_FALSE(accepts(st, ClientFrameKind::auth));
    }
    CHECK_FALSE(accepts(SessionState::hello_sent, ClientFrameKind::hello));
}

TEST_CASE("a client may acknowledge during the replay, not only after it") {
    // Requiring otherwise would force a consumer to buffer an entire backlog
    // before it could confirm any of it.
    CHECK(accepts(SessionState::resuming, ClientFrameKind::ack));
    CHECK(accepts(SessionState::ready, ClientFrameKind::ack));
    // But a second RESUME on a live session is not a way to rewind.
    CHECK_FALSE(accepts(SessionState::resuming, ClientFrameKind::resume));
    CHECK_FALSE(accepts(SessionState::ready, ClientFrameKind::resume));
}

TEST_CASE("GOODBYE is always accepted until the connection is closed") {
    for (const auto st : kStates) {
        if (st == SessionState::closed) {
            CHECK_FALSE(accepts(st, ClientFrameKind::goodbye));
        } else {
            CHECK(accepts(st, ClientFrameKind::goodbye));
        }
    }
}

TEST_CASE("a closing or closed session accepts nothing else") {
    for (const auto f : kFrames) {
        if (f == ClientFrameKind::goodbye) continue;
        CHECK_FALSE(accepts(SessionState::closing, f));
        CHECK_FALSE(accepts(SessionState::closed, f));
    }
}

TEST_CASE("heartbeats are answered only once the server sends them") {
    // The server starts beating at AUTH_OK; before that a HEARTBEAT_ACK is
    // answering something that was never asked.
    CHECK_FALSE(accepts(SessionState::connected, ClientFrameKind::heartbeat_ack));
    CHECK_FALSE(accepts(SessionState::hello_sent, ClientFrameKind::heartbeat_ack));
    CHECK(accepts(SessionState::authenticated, ClientFrameKind::heartbeat_ack));
    CHECK(accepts(SessionState::resuming, ClientFrameKind::heartbeat_ack));
    CHECK(accepts(SessionState::ready, ClientFrameKind::heartbeat_ack));
}

TEST_CASE("signals only reach a session that asked for them") {
    CHECK(can_receive_signals(SessionState::resuming));
    CHECK(can_receive_signals(SessionState::ready));
    // A half-handshaked session must never be handed a trade.
    CHECK_FALSE(can_receive_signals(SessionState::connected));
    CHECK_FALSE(can_receive_signals(SessionState::hello_sent));
    CHECK_FALSE(can_receive_signals(SessionState::authenticated));
    CHECK_FALSE(can_receive_signals(SessionState::closing));
    CHECK_FALSE(can_receive_signals(SessionState::closed));
}

TEST_CASE("the acceptance table has no unnamed holes") {
    // Every (state, frame) pair resolves to a decision. A frame kind added to
    // the protocol without a rule would show up here as an accept-by-accident.
    int accepted = 0;
    for (const auto st : kStates) {
        for (const auto f : kFrames) {
            if (accepts(st, f)) ++accepted;
        }
    }
    // hello(1) + auth(1) + resume(1) + heartbeat_ack(3) + ack(2) + goodbye(6)
    // + query(3: authenticated, resuming, ready)
    CHECK(accepted == 17);
}

TEST_CASE("a query is read-only and is accepted once the client is authenticated") {
    // Protocol 2. It reads the journal and moves no cursor, so it is admitted
    // wherever a heartbeat is -- and refused before authentication, where the
    // caller has not yet proved it may see the journal at all.
    CHECK_FALSE(accepts(SessionState::connected, ClientFrameKind::query));
    CHECK_FALSE(accepts(SessionState::hello_sent, ClientFrameKind::query));
    CHECK(accepts(SessionState::authenticated, ClientFrameKind::query));
    CHECK(accepts(SessionState::resuming, ClientFrameKind::query));
    CHECK(accepts(SessionState::ready, ClientFrameKind::query));
    CHECK_FALSE(accepts(SessionState::closing, ClientFrameKind::query));
    CHECK(client_frame_name(ClientFrameKind::query) == "QUERY");
}

TEST_CASE("state and frame names are stable") {
    CHECK(session_state_name(SessionState::ready) == "READY");
    CHECK(session_state_name(SessionState::hello_sent) == "HELLO_SENT");
    CHECK(client_frame_name(ClientFrameKind::heartbeat_ack) == "HEARTBEAT_ACK");
    CHECK(client_frame_name(ClientFrameKind::none) == "NONE");
}

TEST_CASE("the declared versions are the ones the code uses") {
    // Protocol 2 (queries) over schema 3 (VPS1 signal semantics: the market
    // forecast and setup blocks); protocol 1 is still served, schemas 1-2 are
    // still read from the journal but no longer emitted.
    CHECK(kProtocolVersion == 2);
    CHECK(kMinProtocolVersion == 1);
    CHECK(kSchemaVersion == 3);
    CHECK(protocol_supported(1));
    CHECK(protocol_supported(2));
    CHECK_FALSE(protocol_supported(0));
    CHECK_FALSE(protocol_supported(3));
}
