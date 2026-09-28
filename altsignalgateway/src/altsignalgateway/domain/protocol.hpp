#pragma once

#include <cstdint>
#include <string_view>

namespace altsignalgateway::domain {

// The protocol version this server SPEAKS (its newest) and the oldest one it
// still SERVES. A protocol-1 client is given exactly the protocol-1 contract:
// no query capability is granted to it and it cannot send a frame it does not
// know. Protocol 2 added the query frames and nothing else.
inline constexpr uint32_t kProtocolVersion = 2;
inline constexpr uint32_t kMinProtocolVersion = 1;
// The payload shape this server EMITS. Schema 2 added the macro block, schema 3
// the market forecast and setup blocks (VPS1 signal semantics); a client that
// does not list it in supported_schema_versions is refused at HELLO, because it
// would receive signals it has not declared it can read.
inline constexpr uint32_t kSchemaVersion = 3;

[[nodiscard]] constexpr bool protocol_supported(uint32_t v) noexcept {
    return v >= kMinProtocolVersion && v <= kProtocolVersion;
}

// The WebSocket session state machine, kept free of Asio, OpenSSL and protobuf
// so every transition can be exercised in a unit test rather than against a
// live socket.
//
//   CONNECTED ──HELLO──► HELLO_SENT ──AUTH(ok)──► AUTHENTICATED
//                             │                        │
//                             └──AUTH(fail)──► CLOSING └──RESUME──► RESUMING
//                                                                      │
//                                                       (backlog drained)
//                                                                      ▼
//                                                                    READY
//
// Every state has exactly one set of client frames it accepts. A frame outside
// that set is a protocol violation and closes the connection: a boundary that
// tolerates out-of-order frames is a boundary whose behaviour nobody can state.
enum class SessionState {
    connected,      // TLS + WebSocket handshake done, nothing said yet
    hello_sent,     // HELLO received, HELLO_OK issued, awaiting AUTH
    authenticated,  // AUTH_OK issued, awaiting RESUME
    resuming,       // RESUME accepted, backlog being drained
    ready,          // live stream
    closing,        // GOODBYE/ERROR issued, draining the close
    closed,
};

[[nodiscard]] constexpr std::string_view session_state_name(SessionState s) noexcept {
    switch (s) {
        case SessionState::connected: return "CONNECTED";
        case SessionState::hello_sent: return "HELLO_SENT";
        case SessionState::authenticated: return "AUTHENTICATED";
        case SessionState::resuming: return "RESUMING";
        case SessionState::ready: return "READY";
        case SessionState::closing: return "CLOSING";
        case SessionState::closed: return "CLOSED";
    }
    return "UNKNOWN";
}

// The client frame kinds, mirrored from ClientFrame's oneof so the acceptance
// table does not have to include the generated protobuf headers.
enum class ClientFrameKind {
    none,
    hello,
    auth,
    resume,
    ack,
    heartbeat_ack,
    goodbye,
    query,          // protocol 2
};

[[nodiscard]] constexpr std::string_view client_frame_name(ClientFrameKind k) noexcept {
    switch (k) {
        case ClientFrameKind::none: return "NONE";
        case ClientFrameKind::hello: return "HELLO";
        case ClientFrameKind::auth: return "AUTH";
        case ClientFrameKind::resume: return "RESUME";
        case ClientFrameKind::ack: return "ACK";
        case ClientFrameKind::heartbeat_ack: return "HEARTBEAT_ACK";
        case ClientFrameKind::goodbye: return "GOODBYE";
        case ClientFrameKind::query: return "QUERY";
    }
    return "UNKNOWN";
}

// Is this frame legal in this state?
//
// GOODBYE is accepted everywhere: a client that wants to leave should always be
// able to say so cleanly rather than being forced into an abortive close.
// HEARTBEAT_ACK is accepted from AUTHENTICATED onwards, because that is where
// the server starts sending heartbeats.
// ACK is accepted in RESUMING as well as READY: a client is allowed to
// acknowledge replayed messages as they arrive rather than after the backlog
// completes, and requiring otherwise would force it to buffer the whole replay.
// QUERY is accepted from AUTHENTICATED onwards: it reads, never writes, and a
// consumer that is still draining a backlog may legitimately want the history
// page that tells it what the backlog is. Whether the credential MAY query is
// a capability check made after this table, not part of it.
[[nodiscard]] constexpr bool accepts(SessionState state, ClientFrameKind kind) noexcept {
    if (kind == ClientFrameKind::goodbye) {
        return state != SessionState::closed;
    }
    if (kind == ClientFrameKind::query) {
        return state == SessionState::authenticated || state == SessionState::resuming ||
               state == SessionState::ready;
    }
    switch (state) {
        case SessionState::connected:
            return kind == ClientFrameKind::hello;
        case SessionState::hello_sent:
            return kind == ClientFrameKind::auth;
        case SessionState::authenticated:
            return kind == ClientFrameKind::resume || kind == ClientFrameKind::heartbeat_ack;
        case SessionState::resuming:
            return kind == ClientFrameKind::ack || kind == ClientFrameKind::heartbeat_ack;
        case SessionState::ready:
            return kind == ClientFrameKind::ack || kind == ClientFrameKind::heartbeat_ack;
        case SessionState::closing:
        case SessionState::closed:
            return false;
    }
    return false;
}

// Which states may receive SIGNAL frames. Used by the delivery pump so a
// half-handshaked session can never be handed a trade.
[[nodiscard]] constexpr bool can_receive_signals(SessionState s) noexcept {
    return s == SessionState::resuming || s == SessionState::ready;
}

} // namespace altsignalgateway::domain
