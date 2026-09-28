#pragma once

#include <string>

namespace altsignalgateway::domain {

// Deterministic ids, in the stack's convention: `<prefix><32 hex of sha256>`.
//
// Deterministic rather than random because every id this service mints must
// survive a crash: a redelivered signal has to reproduce the SAME message id,
// or a consumer that de-duplicates on it would see two distinct messages
// carrying one trade.
std::string derive_id(const char* prefix, const std::string& natural_key);

// The transport identity of one journalled signal. Natural key is the upstream
// final_signal_id, so it is stable across restarts, redeliveries and replays.
std::string make_message_id(const std::string& signal_id);

// A per-connection session identity. Random, not derived: it is a nonce
// component of the authentication handshake, and a predictable session id
// would let a captured signature be replayed onto a predicted session.
std::string make_session_id();

} // namespace altsignalgateway::domain
