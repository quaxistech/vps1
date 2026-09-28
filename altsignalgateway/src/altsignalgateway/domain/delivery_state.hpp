#pragma once

#include <optional>
#include <string_view>

namespace altsignalgateway::domain {

// The delivery state machine of one journalled signal.
//
// It is a state machine and not a set of flags because every transition here
// is a claim about durability that a later transition depends on: "SENT" is
// only meaningful if "QUEUED" happened, and "ACKED" is only meaningful if the
// bytes actually left the process. Free-form strings would let a bug write
// ACKED onto a message nobody ever sent, and nothing would notice.
//
//   JOURNALED ──► QUEUED ──► SENT ──► ACKED        (the happy path)
//        │           │         │
//        │           └─────────┴──► JOURNALED      (session lost / restart)
//        │           │         │
//        └───────────┴─────────┴──► EXPIRED        (expires_at passed, never acked)
//        │           │         │
//        └───────────┴─────────┴──► FAILED         (permanently undeliverable)
//
// ACKED and FAILED are terminal. EXPIRED is terminal for delivery; the row
// itself survives for audit until retention removes it.
//
// The same transition table is enforced twice, on purpose: here, so the process
// never attempts an illegal write, and by a PostgreSQL trigger, so a second
// process, a manual UPDATE or a future bug cannot produce a journal whose
// history is impossible. See db/migrations/0001_altsignalgateway_schema.sql.
enum class DeliveryState {
    journaled,
    queued,
    sent,
    acked,
    expired,
    failed,
};

[[nodiscard]] constexpr std::string_view delivery_state_name(DeliveryState s) noexcept {
    switch (s) {
        case DeliveryState::journaled: return "JOURNALED";
        case DeliveryState::queued: return "QUEUED";
        case DeliveryState::sent: return "SENT";
        case DeliveryState::acked: return "ACKED";
        case DeliveryState::expired: return "EXPIRED";
        case DeliveryState::failed: return "FAILED";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::optional<DeliveryState> parse_delivery_state(std::string_view name);

[[nodiscard]] constexpr bool is_terminal(DeliveryState s) noexcept {
    return s == DeliveryState::acked || s == DeliveryState::failed ||
           s == DeliveryState::expired;
}

// True when a signal in this state is still owed to a client.
[[nodiscard]] constexpr bool is_deliverable(DeliveryState s) noexcept {
    return s == DeliveryState::journaled || s == DeliveryState::queued ||
           s == DeliveryState::sent;
}

// The single source of truth for what may follow what.
//
// ACKED -> ACKED is allowed and is a no-op: at-least-once delivery means a
// client can legitimately ACK the same message twice (its own ACK was lost, or
// it reconnected before the cursor advanced), and treating the second one as an
// illegal transition would drop a connection over correct client behaviour.
[[nodiscard]] constexpr bool is_legal_transition(DeliveryState from, DeliveryState to) noexcept {
    if (from == to) return to == DeliveryState::acked;
    switch (from) {
        case DeliveryState::journaled:
            return to == DeliveryState::queued || to == DeliveryState::expired ||
                   to == DeliveryState::failed;
        case DeliveryState::queued:
            return to == DeliveryState::sent || to == DeliveryState::journaled ||
                   to == DeliveryState::expired || to == DeliveryState::failed;
        case DeliveryState::sent:
            return to == DeliveryState::acked || to == DeliveryState::journaled ||
                   to == DeliveryState::expired || to == DeliveryState::failed;
        case DeliveryState::acked:
        case DeliveryState::expired:
        case DeliveryState::failed:
            return false;
    }
    return false;
}

} // namespace altsignalgateway::domain
