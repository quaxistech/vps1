#include "altsignalgateway/components/session.hpp"

#include "altsignalgateway/core/codec.hpp"
#include "altsignalgateway/core/logger.hpp"
#include "altsignalgateway/core/time.hpp"
#include "altsignalgateway/domain/ids.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <utility>

namespace altsignalgateway::components {

namespace pb = altsignalgateway::v1;
using domain::SessionState;

namespace {

// The client's contribution of entropy to the signed handshake. 16 bytes is the
// smallest size at which a birthday collision across the lifetime of a
// deployment is not worth reasoning about; the server's own nonce is the part
// that actually prevents replay, so this is a floor rather than a target.
constexpr size_t kMinClientNonceBytes = 16;
// The server's single-use challenge. 32 bytes = the full output width of the
// SHA-256 the signature is computed with; a shorter nonce would be the weakest
// component of the scheme.
constexpr size_t kServerNonceBytes = 32;
// Heartbeat nonces exist to bind an answer to the beat that asked for it, over
// a connection that is already authenticated and encrypted. 8 bytes makes a
// stale answer distinguishable; it is not a security boundary.
constexpr size_t kHeartbeatNonceBytes = 8;
// How often the session timer runs. Every deadline this service enforces —
// handshake, resume, heartbeat, acknowledgement — is expressed in whole
// seconds, so a one-second tick is the coarsest interval that can still honour
// all of them to their stated precision.
constexpr auto kTimerTick = std::chrono::seconds(1);
// An upper bound on the client_id a HELLO may claim. It is used as a map key
// and appears in every log record for the session; 128 bytes is far above any
// legitimate identity and far below anything that could be used to bloat a log.
constexpr size_t kMaxClientIdBytes = 128;

domain::ClientFrameKind kind_of(const pb::ClientFrame& f) {
    switch (f.body_case()) {
        case pb::ClientFrame::kHello: return domain::ClientFrameKind::hello;
        case pb::ClientFrame::kAuth: return domain::ClientFrameKind::auth;
        case pb::ClientFrame::kResume: return domain::ClientFrameKind::resume;
        case pb::ClientFrame::kAck: return domain::ClientFrameKind::ack;
        case pb::ClientFrame::kHeartbeatAck: return domain::ClientFrameKind::heartbeat_ack;
        case pb::ClientFrame::kGoodbye: return domain::ClientFrameKind::goodbye;
        case pb::ClientFrame::kQuery: return domain::ClientFrameKind::query;
        case pb::ClientFrame::BODY_NOT_SET: break;
    }
    return domain::ClientFrameKind::none;
}

} // namespace

Session::Session(tcp::socket&& socket, ssl::context& ssl_ctx, SessionDeps deps)
    : ws_(std::move(socket), ssl_ctx),
      strand_(ws_.get_executor()),
      timer_(strand_),
      deps_(std::move(deps)),
      session_id_(domain::make_session_id()) {
    // Frame and message bounds are set BEFORE the handshake, so an oversized
    // first frame is rejected by Beast rather than buffered by us.
    ws_.read_message_max(static_cast<std::size_t>(deps_.ws.max_message_bytes));
    ws_.binary(true);
    ws_.auto_fragment(false);
    try {
        peer_ = beast::get_lowest_layer(ws_).socket().remote_endpoint().address().to_string();
    } catch (const std::exception&) {
        peer_ = "unknown";
    }
}

Session::~Session() = default;

int64_t Session::now_ms() const { return timeutil::now_ms(); }

std::string Session::client_id() const {
    std::lock_guard lock(identity_mu_);
    return client_id_;
}

void Session::run() {
    connected_at_ms_ = now_ms();
    // A peer that has just failed authentication repeatedly is refused before
    // any cryptography happens: the cheapest possible answer to a brute force.
    if (deps_.is_locked_out && deps_.is_locked_out(peer_)) {
        log::warn("connection_refused_locked_out", {{"peer", peer_}});
        beast::get_lowest_layer(ws_).close();
        return;
    }
    // Beast's own timeout covers the TCP/TLS phase, where our application
    // heartbeat does not exist yet.
    beast::get_lowest_layer(ws_).expires_after(
        std::chrono::seconds(deps_.auth.handshake_timeout_sec));
    auto self = shared_from_this();
    ws_.next_layer().async_handshake(
        ssl::stream_base::server,
        asio::bind_executor(strand_, [self](beast::error_code ec) { self->on_tls_handshake(ec); }));
}

void Session::on_tls_handshake(beast::error_code ec) {
    if (ec) {
        log::info("tls_handshake_failed", {{"peer", peer_}, {"error", ec.message()}});
        close_now("tls_handshake_failed");
        return;
    }
    ws_.set_option(websocket::stream_base::decorator([v = deps_.server_version](
                                                         websocket::response_type& res) {
        res.set(beast::http::field::server, "altsignalgateway/" + v);
    }));
    auto self = shared_from_this();
    ws_.async_accept(asio::bind_executor(
        strand_, [self](beast::error_code e) { self->on_ws_accept(e); }));
}

void Session::on_ws_accept(beast::error_code ec) {
    if (ec) {
        log::info("websocket_accept_failed", {{"peer", peer_}, {"error", ec.message()}});
        close_now("websocket_accept_failed");
        return;
    }
    // From here on the application heartbeat governs liveness, so Beast's own
    // deadline is disarmed: leaving it armed would kill a healthy idle session
    // that simply has no signals to receive, which is the normal state of this
    // endpoint.
    beast::get_lowest_layer(ws_).expires_never();
    if (!deps_.attach(shared_from_this())) {
        send_error(pb::ERR_TOO_MANY_CLIENTS, "endpoint is at its configured client limit", true);
        return;
    }
    log::info("client_connected", {{"session_id", session_id_}, {"peer", peer_}});
    arm_timer();
    do_read();
}

void Session::do_read() {
    if (closed_.load(std::memory_order_acquire)) return;
    auto self = shared_from_this();
    ws_.async_read(buffer_, asio::bind_executor(strand_, [self](beast::error_code ec,
                                                                std::size_t n) {
        self->on_read(ec, n);
    }));
}

void Session::on_read(beast::error_code ec, std::size_t bytes) {
    if (ec == websocket::error::closed) {
        close_now("client_closed");
        return;
    }
    if (ec) {
        // Beast refuses an oversized message BEFORE buffering it — that is what
        // read_message_max, set on the stream before the handshake, is for. The
        // bound is therefore enforced by the transport rather than checked
        // after the fact, which is the only ordering that actually protects
        // memory.
        if (ec == websocket::error::buffer_overflow || ec == websocket::error::message_too_big) {
            log::warn("oversized_message", {{"session_id", session_id_},
                                            {"client_id", client_id()},
                                            {"limit_bytes", deps_.ws.max_message_bytes}});
            send_error(pb::ERR_MESSAGE_TOO_LARGE, "message exceeds the configured limit", true);
            return;
        }
        log::info("read_failed", {{"session_id", session_id_}, {"error", ec.message()}});
        close_now("read_failed:" + ec.message());
        return;
    }
    if (!ws_.got_binary()) {
        // The protocol is protobuf over binary frames. A text frame is either a
        // different protocol or a probe.
        send_error(pb::ERR_MALFORMED_FRAME, "expected a binary frame", true);
        return;
    }
    const auto data = buffer_.cdata();
    const std::string_view view(static_cast<const char*>(data.data()), data.size());
    auto frame = messaging::decode_client_frame(view);
    buffer_.consume(buffer_.size());
    if (!frame) {
        send_error(pb::ERR_MALFORMED_FRAME, "frame is not a valid ClientFrame", true);
        return;
    }
    handle_frame(*frame);
    // No new read once a fatal error is on its way out.
    const auto after = state();
    if (!closed_.load(std::memory_order_acquire) && after != SessionState::closing &&
        after != SessionState::closed) {
        do_read();
    }
}

void Session::handle_frame(const pb::ClientFrame& frame) {
    const auto kind = kind_of(frame);
    const auto st = state();
    // A frame that was already on the wire when the server sent a fatal error
    // is not a protocol violation: it crossed the error frame in flight and the
    // client has done nothing wrong. Reporting it as one would also OVERWRITE
    // the real close reason in the session audit, so an operator investigating
    // a slow consumer would find ERR_PROTOCOL_VIOLATION where the truth was
    // ERR_TIMEOUT.
    if (st == SessionState::closing || st == SessionState::closed) {
        log::debug("frame_after_close", {{"session_id", session_id_},
                                         {"client_id", client_id()},
                                         {"frame", std::string(domain::client_frame_name(kind))}});
        return;
    }
    if (kind == domain::ClientFrameKind::none) {
        send_error(pb::ERR_MALFORMED_FRAME, "frame carries no body", true);
        return;
    }
    if (!domain::accepts(st, kind)) {
        log::warn("protocol_violation", {{"session_id", session_id_},
                                         {"client_id", client_id()},
                                         {"state", std::string(domain::session_state_name(st))},
                                         {"frame", std::string(domain::client_frame_name(kind))}});
        send_error(pb::ERR_PROTOCOL_VIOLATION,
                   std::string(domain::client_frame_name(kind)) + " is not accepted in state " +
                       std::string(domain::session_state_name(st)),
                   true);
        return;
    }
    switch (kind) {
        case domain::ClientFrameKind::hello: handle_hello(frame.hello()); break;
        case domain::ClientFrameKind::auth: handle_auth(frame.auth()); break;
        case domain::ClientFrameKind::resume: handle_resume(frame.resume()); break;
        case domain::ClientFrameKind::ack: handle_ack(frame.ack()); break;
        case domain::ClientFrameKind::heartbeat_ack: handle_heartbeat_ack(frame.heartbeat_ack()); break;
        case domain::ClientFrameKind::query: handle_query(frame.query()); break;
        case domain::ClientFrameKind::goodbye:
            log::info("client_goodbye", {{"session_id", session_id_},
                                         {"client_id", client_id()},
                                         {"reason", frame.goodbye().reason()}});
            close_now("client_goodbye");
            break;
        case domain::ClientFrameKind::none: break;
    }
}

void Session::handle_hello(const pb::Hello& hello) {
    // The client's version is what the session speaks from here on: a
    // protocol-1 client is served the protocol-1 contract (no query capability
    // is granted to it), a protocol-2 client the full one.
    if (!domain::protocol_supported(hello.protocol_version())) {
        send_error(pb::ERR_UNSUPPORTED_PROTOCOL_VERSION,
                   "server serves protocol versions " + std::to_string(domain::kMinProtocolVersion) +
                       ".." + std::to_string(domain::kProtocolVersion),
                   true);
        return;
    }
    // A client that cannot read the schema this server produces would receive
    // trades it cannot parse. Refusing at HELLO is the honest answer.
    bool schema_ok = false;
    for (const auto v : hello.supported_schema_versions()) {
        if (v == domain::kSchemaVersion) schema_ok = true;
    }
    if (!schema_ok) {
        send_error(pb::ERR_UNSUPPORTED_SCHEMA_VERSION,
                   "server emits payload schema version " + std::to_string(domain::kSchemaVersion),
                   true);
        return;
    }
    if (hello.client_id().empty() || hello.client_id().size() > kMaxClientIdBytes) {
        send_error(pb::ERR_PROTOCOL_VIOLATION,
                   "client_id must be 1.." + std::to_string(kMaxClientIdBytes) + " bytes", true);
        return;
    }
    // The client nonce goes into the signed string; a short or absent one would
    // let the client contribute no entropy of its own to the exchange.
    if (hello.client_nonce().size() < kMinClientNonceBytes) {
        send_error(pb::ERR_PROTOCOL_VIOLATION,
                   "client_nonce must be at least " + std::to_string(kMinClientNonceBytes) +
                       " bytes",
                   true);
        return;
    }

    claimed_client_id_ = hello.client_id();
    client_version_ = hello.client_version();
    client_protocol_version_ = hello.protocol_version();
    client_nonce_ = hello.client_nonce();
    server_nonce_ = domain::random_bytes(kServerNonceBytes);

    pb::ServerFrame out;
    auto* ok = out.mutable_hello_ok();
    ok->set_protocol_version(client_protocol_version_);
    ok->set_schema_version(domain::kSchemaVersion);
    ok->set_server_version(deps_.server_version);
    ok->set_session_id(session_id_);
    ok->set_auth_scheme(std::string(domain::kAuthScheme));
    ok->set_server_nonce(server_nonce_);
    ok->set_server_time_ms(now_ms());
    ok->set_auth_timeout_ms(static_cast<int64_t>(deps_.auth.handshake_timeout_sec) * 1000);
    ok->set_max_clock_skew_ms(static_cast<int64_t>(deps_.auth.max_clock_skew_sec) * 1000);
    send_control(out);
    set_state(SessionState::hello_sent);
}

void Session::handle_auth(const pb::Auth& auth) {
    domain::AuthRequest req;
    req.protocol_version = client_protocol_version_;
    req.client_id = claimed_client_id_;
    req.key_id = auth.key_id();
    req.session_id = session_id_;
    req.server_nonce = server_nonce_;
    req.echoed_nonce = auth.server_nonce();
    req.client_nonce = client_nonce_;
    req.timestamp_ms = auth.timestamp_ms();
    req.signature = auth.signature();

    const domain::AuthorisedClient* client = nullptr;
    const auto failure = domain::verify_auth(
        *deps_.registry, req, now_ms(),
        static_cast<int64_t>(deps_.auth.max_clock_skew_sec) * 1000, &client);

    if (failure != domain::AuthFailure::none) {
        // The operator learns which check failed; the caller learns only that
        // it failed. Telling a caller *why* its signature was rejected is a
        // free oracle for refining an attack.
        log::warn("authentication_failed",
                  {{"session_id", session_id_},
                   {"peer", peer_},
                   {"claimed_client_id", claimed_client_id_},
                   {"key_id", auth.key_id()},
                   {"reason", std::string(domain::auth_failure_name(failure))}});
        if (deps_.note_auth_failure) deps_.note_auth_failure(peer_);
        send_error(pb::ERR_AUTH_FAILED, "authentication failed", true);
        return;
    }

    // The single-use server nonce is consumed here. A second AUTH on the same
    // connection cannot reuse it, and the protocol state machine already
    // refuses AUTH outside HELLO_SENT.
    server_nonce_.assign(server_nonce_.size(), '\0');
    client_ = client;
    {
        std::lock_guard lock(identity_mu_);
        client_id_ = client->client_id;
    }
    authenticated_at_ms_ = now_ms();
    last_heartbeat_ack_ms_ = authenticated_at_ms_;
    if (deps_.note_auth_success) deps_.note_auth_success(peer_);
    // Duplicate-session policy, applied under the registry's lock so two
    // simultaneous connections cannot both decide they are the newest.
    if (deps_.enforce_client_limit) {
        deps_.enforce_client_limit(client->client_id, session_id_, client->max_sessions);
    }

    JournalOp open;
    open.kind = JournalOp::Kind::open_session;
    open.session_id = session_id_;
    open.client_id = client->client_id;
    open.key_id = client->key_id;
    open.peer = peer_;
    open.protocol_version = static_cast<int32_t>(client_protocol_version_);
    open.text = client_version_;
    open.at_ms = authenticated_at_ms_;
    deps_.submit(std::move(open));

    log::info("client_authenticated", {{"session_id", session_id_},
                                       {"client_id", client->client_id},
                                       {"key_id", client->key_id},
                                       {"peer", peer_},
                                       {"client_version", client_version_}});

    // The durable cursor is read on the database thread and the answer comes
    // back on this strand.
    auto self = shared_from_this();
    deps_.load_cursor(client->client_id, [self](int64_t cursor, int64_t head) {
        asio::post(self->strand_, [self, cursor, head] {
            if (self->closed_.load(std::memory_order_acquire)) return;
            pb::ServerFrame out;
            auto* ok = out.mutable_auth_ok();
            ok->set_client_id(self->client_id());
            ok->set_session_id(self->session_id_);
            for (const auto cap : self->client_->capabilities) {
                switch (cap) {
                    case domain::Capability::receive: ok->add_capabilities(pb::CAP_RECEIVE_SIGNALS); break;
                    case domain::Capability::ack: ok->add_capabilities(pb::CAP_ACK); break;
                    case domain::Capability::resume: ok->add_capabilities(pb::CAP_RESUME); break;
                    case domain::Capability::query:
                        // A protocol-1 client has no query frame to send, so
                        // advertising the capability would promise what it
                        // cannot use.
                        if (self->client_protocol_version_ >= 2) ok->add_capabilities(pb::CAP_QUERY);
                        break;
                }
            }
            ok->set_heartbeat_interval_ms(
                static_cast<int64_t>(self->deps_.ws.heartbeat_interval_sec) * 1000);
            ok->set_heartbeat_timeout_ms(
                static_cast<int64_t>(self->deps_.ws.heartbeat_timeout_sec) * 1000);
            ok->set_max_message_bytes(self->deps_.ws.max_message_bytes);
            ok->set_last_acked_sequence(cursor);
            ok->set_journal_head_sequence(head);
            ok->set_resume_timeout_ms(
                static_cast<int64_t>(self->deps_.ws.resume_timeout_sec) * 1000);
            self->send_control(out);
            self->set_state(SessionState::authenticated);
        });
    });
}

void Session::handle_resume(const pb::Resume& resume) {
    if (client_ == nullptr || !client_->has(domain::Capability::resume)) {
        send_error(pb::ERR_NOT_AUTHORIZED, "this client may not resume", true);
        return;
    }
    if (resume.mode() == pb::RESUME_MODE_UNSPECIFIED) {
        // A resume intent is never guessed. "Start where I left off" and "send
        // me everything" differ by an entire journal.
        send_error(pb::ERR_RESUME_REJECTED, "resume mode must be stated explicitly", true);
        return;
    }

    auto self = shared_from_this();
    deps_.load_cursor(client_->client_id, [self, resume](int64_t cursor, int64_t head) {
        asio::post(self->strand_, [self, resume, cursor, head] {
            if (self->closed_.load(std::memory_order_acquire)) return;
            if (self->state() != SessionState::authenticated) return;
            int64_t from = 0;
            switch (resume.mode()) {
                case pb::RESUME_FROM_SEQUENCE: {
                    const int64_t asked = resume.last_acked_sequence();
                    if (asked < 0) {
                        self->send_error(pb::ERR_INVALID_SEQUENCE,
                                         "last_acked_sequence must not be negative", true);
                        return;
                    }
                    // A client claiming a position ahead of the journal is
                    // either talking to the wrong deployment or has corrupted
                    // state. Nothing destructive follows: the request is
                    // refused and the client keeps whatever it has.
                    if (asked > head) {
                        self->send_error(pb::ERR_INVALID_SEQUENCE,
                                         "last_acked_sequence " + std::to_string(asked) +
                                             " is ahead of the journal head " +
                                             std::to_string(head),
                                         true);
                        return;
                    }
                    // The server's own record wins when the client asks to go
                    // BACKWARDS. Re-sending acknowledged trades on a client's
                    // say-so is how a duplicate order gets placed, and the
                    // cursor is durable precisely so delivery does not depend
                    // on the client's memory.
                    from = std::max(asked, cursor);
                    break;
                }
                case pb::RESUME_LIVE_ONLY:
                    from = head;
                    break;
                case pb::RESUME_FROM_START:
                    from = 0;
                    break;
                default:
                    self->send_error(pb::ERR_RESUME_REJECTED, "unsupported resume mode", true);
                    return;
            }

            // Phase two: ask what is actually owed above `from`, so RESUME_OK
            // carries measured counts rather than a placeholder the client
            // would have to guess the meaning of.
            self->deps_.query_backlog(from, [self, resume, from, head, cursor](int64_t deliverable,
                                                                               int64_t expired) {
                asio::post(self->strand_, [self, resume, from, head, cursor, deliverable, expired] {
                    if (self->closed_.load(std::memory_order_acquire)) return;
                    if (self->state() != SessionState::authenticated) return;

                    self->set_scan_cursor(from);
                    // The cursor baseline for this session. Everything at or
                    // below it is already settled for this client.
                    self->examined_to_ = from;
                    self->cursor_watermark_ = from;
                    self->outstanding_seqs_.clear();
                    self->acks_since_advance_ = 0;
                    // "Live only" means "start me at the head for THIS session".
                    // Persisting a cursor at the head would turn a diagnostic
                    // connection into a permanent abandonment of everything
                    // below it — and a consumer that ran one with production
                    // credentials would silently discard undelivered trades.
                    // The session reads from the head; the durable cursor is
                    // left exactly where acknowledgement put it.
                    self->persist_cursor_ = resume.mode() != pb::RESUME_LIVE_ONLY;
                    self->resume_target_.store(head, std::memory_order_release);
                    self->replaying_.store(true, std::memory_order_release);
                    self->set_state(SessionState::resuming);

                    pb::ServerFrame out;
                    auto* ok = out.mutable_resume_ok();
                    ok->set_from_sequence_exclusive(from);
                    ok->set_journal_head_sequence(head);
                    ok->set_backlog_count(deliverable);
                    ok->set_skipped_expired(expired);
                    self->send_control(out);

                    JournalOp op;
                    op.kind = JournalOp::Kind::resume_noted;
                    op.session_id = self->session_id_;
                    op.text = pb::ResumeMode_Name(resume.mode());
                    op.aux = from;
                    op.aux2 = deliverable;
                    self->deps_.submit(std::move(op));

                    log::info("resume_requested",
                              {{"session_id", self->session_id_},
                               {"client_id", self->client_id()},
                               {"mode", pb::ResumeMode_Name(resume.mode())},
                               {"requested", resume.last_acked_sequence()},
                               {"server_cursor", cursor},
                               {"from_exclusive", from},
                               {"journal_head", head},
                               {"backlog", deliverable},
                               {"expired_in_range", expired}});
                    if (self->deps_.wake_pump) self->deps_.wake_pump();
                });
            });
        });
    });
}

void Session::handle_ack(const pb::Ack& ack) {
    if (client_ == nullptr || !client_->has(domain::Capability::ack)) {
        send_error(pb::ERR_NOT_AUTHORIZED, "this client may not acknowledge", true);
        return;
    }
    auto it = in_flight_.find(ack.message_id());
    if (it == in_flight_.end()) {
        // A duplicate or late ACK is legal under at-least-once delivery and is
        // NOT a protocol violation: the client's own retry logic may repeat it
        // after a reconnect. It is logged and ignored.
        log::debug("ack_for_unknown_message", {{"session_id", session_id_},
                                               {"client_id", client_id()},
                                               {"message_id", ack.message_id()}});
        return;
    }
    // The ACK must describe the message the server actually sent. A mismatch
    // means the client's bookkeeping and the server's disagree, and advancing a
    // cursor on it would skip a trade.
    if (it->second.signal_id != ack.signal_id() || it->second.sequence != ack.sequence()) {
        log::warn("ack_mismatch", {{"session_id", session_id_},
                                   {"client_id", client_id()},
                                   {"message_id", ack.message_id()},
                                   {"expected_signal_id", it->second.signal_id},
                                   {"acked_signal_id", ack.signal_id()},
                                   {"expected_sequence", it->second.sequence},
                                   {"acked_sequence", ack.sequence()}});
        send_error(pb::ERR_INVALID_SEQUENCE,
                   "ACK does not match the message the server sent", true);
        return;
    }

    const int64_t at = now_ms();
    JournalOp op;
    op.kind = JournalOp::Kind::acked;
    op.signal_id = it->second.signal_id;
    op.sequence = it->second.sequence;
    op.client_id = client_id();
    op.session_id = session_id_;
    op.at_ms = at;
    deps_.submit(std::move(op));

    const int64_t acked_seq = it->second.sequence;
    in_flight_.erase(it);
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    outstanding_seqs_.erase(acked_seq);
    ++acked_count_;
    ++acks_since_advance_;
    oldest_unacked_ms_ = in_flight_.empty()
                             ? 0
                             : std::min_element(in_flight_.begin(), in_flight_.end(),
                                                [](const auto& a, const auto& b) {
                                                    return a.second.sent_at_ms < b.second.sent_at_ms;
                                                })
                                   ->second.sent_at_ms;
    maybe_advance_cursor();
    if (deps_.wake_pump) deps_.wake_pump();
}

void Session::maybe_advance_cursor() {
    // The durable cursor is the highest sequence below which this client is
    // owed NOTHING: everything at or under it was either acknowledged or was
    // never offered (expired, or permanently undeliverable).
    //
    // It is deliberately NOT the highest acknowledged sequence. Acknowledging
    // in batches is legitimate consumer behaviour — commit every N trades, then
    // acknowledge the Nth — and a cursor that followed the highest ACK would
    // step over the N-1 messages in between. On the next RESUME those would sit
    // below the cursor and never be delivered again: silent loss of a trade,
    // under a perfectly well-behaved client.
    const int64_t ceiling =
        outstanding_seqs_.empty() ? examined_to_ : (*outstanding_seqs_.begin() - 1);
    if (ceiling <= cursor_watermark_) return;

    cursor_watermark_ = ceiling;
    // A live-only session tracks its watermark locally so its own bookkeeping
    // stays coherent, but never writes it: see persist_cursor_.
    if (!persist_cursor_) return;
    JournalOp op;
    op.kind = JournalOp::Kind::cursor_advance;
    op.client_id = client_id();
    op.session_id = session_id_;
    op.sequence = cursor_watermark_;
    op.aux = acks_since_advance_;
    op.at_ms = now_ms();
    acks_since_advance_ = 0;
    deps_.submit(std::move(op));
}

void Session::handle_heartbeat_ack(const pb::HeartbeatAck& hb) {
    // The nonce proves the answer belongs to the beat that was sent, not to an
    // older one a stalled client is still working through.
    if (!heartbeat_nonce_.empty() && hb.nonce() != heartbeat_nonce_) {
        log::debug("stale_heartbeat_ack", {{"session_id", session_id_}});
        return;
    }
    last_heartbeat_ack_ms_ = now_ms();
}

void Session::send_query_error(const std::string& request_id, pb::ErrorCode code,
                               const std::string& message) {
    pb::ServerFrame out;
    auto* resp = out.mutable_query_response();
    resp->set_request_id(request_id);
    resp->mutable_error()->set_code(code);
    resp->mutable_error()->set_message(message);
    send_control(out);
}

void Session::handle_query(const pb::QueryRequest& query) {
    // A protocol-1 session cannot have sent this frame at all; if it somehow
    // did, it is a protocol violation, not a query.
    if (client_protocol_version_ < 2) {
        send_error(pb::ERR_PROTOCOL_VIOLATION, "QUERY requires protocol version 2", true);
        return;
    }
    if (client_ == nullptr || !client_->has(domain::Capability::query)) {
        send_error(pb::ERR_NOT_AUTHORIZED, "credential lacks the query capability", true);
        return;
    }
    // Bounds, all answered in-band and none of them fatal: a consumer that
    // asks too fast is told so and keeps its signal stream.
    if (query_inflight_) {
        send_query_error(query.request_id(), pb::ERR_QUERY_LIMIT, "one query in flight per session");
        return;
    }
    const int64_t now = now_ms();
    while (!query_times_ms_.empty() && query_times_ms_.front() < now - 60'000) {
        query_times_ms_.pop_front();
    }
    if (static_cast<int32_t>(query_times_ms_.size()) >= deps_.query.rate_per_minute) {
        send_query_error(query.request_id(), pb::ERR_QUERY_LIMIT,
                         "rate limit: " + std::to_string(deps_.query.rate_per_minute) +
                             " queries per minute per session");
        return;
    }
    if (const auto bad = validate_query(query, deps_.query)) {
        send_query_error(query.request_id(), bad->code(), bad->message());
        return;
    }
    query_times_ms_.push_back(now);
    query_inflight_ = true;

    QueryJob job;
    job.session_id = session_id_;
    job.client_id = client_id();
    job.request = query;
    auto self = shared_from_this();
    job.reply = [self](pb::QueryResponse response) {
        asio::post(self->strand_, [self, response = std::move(response)]() mutable {
            self->query_inflight_ = false;
            if (self->closed_.load(std::memory_order_acquire)) return;
            pb::ServerFrame out;
            *out.mutable_query_response() = std::move(response);
            self->send_control(out);
        });
    };
    if (!deps_.run_query || !deps_.run_query(std::move(job))) {
        query_inflight_ = false;
        send_query_error(query.request_id(), pb::ERR_QUERY_LIMIT,
                         "query queue is full; retry shortly");
    }
}

// ---------------------------------------------------------------------------
// delivery
// ---------------------------------------------------------------------------

int32_t Session::delivery_credit() const {
    const auto st = state();
    if (!domain::can_receive_signals(st)) return 0;
    if (closed_.load(std::memory_order_acquire)) return 0;
    const int32_t used = outstanding_.load(std::memory_order_acquire);
    const int32_t limit = deps_.ws.max_queued_messages;
    return used >= limit ? 0 : limit - used;
}

void Session::deliver(std::vector<OutboundSignal> batch, int64_t examined_to) {
    // The counter is raised HERE, on the producer's thread, so two pump passes
    // cannot both see credit for the same slot.
    outstanding_.fetch_add(static_cast<int32_t>(batch.size()), std::memory_order_acq_rel);
    auto self = shared_from_this();
    asio::post(strand_, [self, batch = std::move(batch), examined_to]() mutable {
        if (self->closed_.load(std::memory_order_acquire)) {
            self->outstanding_.fetch_sub(static_cast<int32_t>(batch.size()),
                                         std::memory_order_acq_rel);
            // The pump marked these QUEUED before handing them over, and this
            // session closed in the gap. close_now has already run, so its
            // requeue cannot cover them — they would sit in QUEUED, invisible to
            // every later session, until the next restart's recovery pass. So
            // they are returned here instead.
            if (!batch.empty()) {
                JournalOp op;
                op.kind = JournalOp::Kind::requeue;
                op.signal_ids.reserve(batch.size());
                for (const auto& s : batch) op.signal_ids.push_back(s.signal_id);
                op.client_id = self->client_id();
                op.session_id = self->session_id_;
                op.text = "session closed before the batch reached it";
                op.at_ms = self->now_ms();
                self->deps_.submit(std::move(op));
            }
            return;
        }
        // Both halves are applied on the strand, in this order, so the cursor
        // computation can never see a moved watermark without also seeing the
        // sequences that moved it.
        for (const auto& s : batch) self->outstanding_seqs_.insert(s.sequence);
        if (examined_to > self->examined_to_) self->examined_to_ = examined_to;
        for (auto& s : batch) {
            self->enqueue(std::move(s.frame), true, s.sequence, s.signal_id, s.message_id,
                          s.attempt, s.replay);
        }
        // An empty batch with a moved watermark is the normal case for a window
        // in which everything had expired; it still lets the cursor advance.
        self->maybe_advance_cursor();
    });
}

void Session::finish_replay(int64_t last_sequence, int64_t journal_head, int64_t delivered) {
    auto self = shared_from_this();
    asio::post(strand_, [self, last_sequence, journal_head, delivered] {
        if (self->closed_.load(std::memory_order_acquire)) return;
        if (self->state() != SessionState::resuming) return;
        self->replaying_.store(false, std::memory_order_release);
        self->replayed_count_ = delivered;

        pb::ServerFrame done;
        auto* rc = done.mutable_resume_complete();
        rc->set_last_sequence(last_sequence);
        rc->set_delivered(delivered);
        self->send_control(done);

        pb::ServerFrame ready;
        auto* r = ready.mutable_ready();
        r->set_server_time_ms(self->now_ms());
        r->set_journal_head_sequence(journal_head);
        self->send_control(ready);
        self->set_state(SessionState::ready);
        log::info("session_ready", {{"session_id", self->session_id_},
                                    {"client_id", self->client_id()},
                                    {"replayed", delivered},
                                    {"replay_ended_at", last_sequence},
                                    {"journal_head", journal_head}});
    });
}

void Session::enqueue(std::string frame, bool is_signal, int64_t sequence,
                      const std::string& signal_id, const std::string& message_id, int32_t attempt,
                      bool replay) {
    // The bound covers control frames too. A socket that has stopped draining
    // accumulates heartbeats, and that is exactly the condition this limit is
    // meant to catch — a client whose TCP window is closed looks healthy at the
    // protocol level right up until the process runs out of memory.
    // Once the session is on its way out, a frame that has not been written is
    // dropped rather than queued behind an error nobody will read.
    const bool shutting_down = !close_reason_.empty() || closed_.load(std::memory_order_acquire);
    const bool overflow =
        static_cast<int32_t>(queue_.size()) >= deps_.ws.max_queued_messages ||
        queued_bytes_ + frame.size() > deps_.ws.max_queued_bytes;
    if (shutting_down || overflow) {
        if (is_signal) {
            // The pump already marked this signal QUEUED in the journal. It
            // never reached the socket, so its credit is released and it is
            // recorded for the requeue that close_now performs — otherwise it
            // would sit in QUEUED until the next restart, invisible to every
            // later session because the pump only reads ABOVE a cursor.
            //
            // It deliberately STAYS in outstanding_seqs_: the client never saw
            // it, so the durable cursor must not move past it.
            undelivered_.push_back(signal_id);
            outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        }
        if (overflow && !shutting_down) {
            log::warn("outbound_queue_overflow", {{"session_id", session_id_},
                                                  {"client_id", client_id()},
                                                  {"queued", queue_.size()},
                                                  {"queued_bytes", queued_bytes_}});
            // Reported once: the rest of the batch takes the shutting_down
            // branch above rather than pushing an error frame each.
            send_error(pb::ERR_QUEUE_OVERFLOW,
                       "outbound queue limit reached; reconnect and RESUME", true);
        }
        return;
    }
    queued_bytes_ += frame.size();
    queue_.push_back(Pending{std::move(frame), is_signal, sequence, signal_id, message_id, attempt,
                             replay});
    write_next();
}

void Session::write_next() {
    if (writing_ || queue_.empty() || closed_.load(std::memory_order_acquire)) return;
    writing_ = true;
    const auto& front = queue_.front();
    auto self = shared_from_this();
    ws_.async_write(asio::buffer(front.frame),
                    asio::bind_executor(strand_, [self](beast::error_code ec, std::size_t n) {
                        self->on_write(ec, n);
                    }));
}

void Session::on_write(beast::error_code ec, std::size_t /*bytes*/) {
    writing_ = false;
    if (ec) {
        log::info("write_failed", {{"session_id", session_id_},
                                   {"client_id", client_id()},
                                   {"error", ec.message()}});
        close_now("write_failed:" + ec.message());
        return;
    }
    if (close_pending_) {
        // close_now ran while this write was in flight; the stream is free now.
        finish_close();
        return;
    }
    if (queue_.empty()) return;   // defensive: a close raced the completion
    Pending done = std::move(queue_.front());
    queue_.pop_front();
    queued_bytes_ -= done.frame.size();

    if (done.is_signal) {
        const int64_t at = now_ms();
        in_flight_.emplace(done.message_id,
                           InFlight{done.sequence, done.signal_id, at});
        if (oldest_unacked_ms_ == 0) oldest_unacked_ms_ = at;
        ++delivered_count_;

        // The durable SENT transition is recorded only after the bytes have
        // actually been handed to the transport. Recording it before the write
        // would let a signal that never left the process be indistinguishable
        // from one that did.
        JournalOp op;
        op.kind = JournalOp::Kind::sent;
        op.signal_id = done.signal_id;
        op.sequence = done.sequence;
        op.client_id = client_id();
        op.session_id = session_id_;
        op.attempt = done.attempt;
        op.replay = done.replay;
        op.at_ms = at;
        deps_.submit(std::move(op));
    } else {
        // A control frame carrying a fatal error is the last thing this session
        // says.
        if (!close_reason_.empty() && queue_.empty()) {
            close_now(close_reason_);
            return;
        }
    }
    write_next();
}

void Session::send_control(const pb::ServerFrame& frame) {
    auto bytes = messaging::encode_server_frame(frame);
    if (!bytes) {
        log::error("server_frame_encode_failed",
                   {{"session_id", session_id_}, {"message", bytes.error().message}});
        close_now("frame_encode_failed");
        return;
    }
    enqueue(std::move(*bytes), false, 0, {}, {}, 0, false);
}

void Session::send_error(pb::ErrorCode code, const std::string& message, bool fatal) {
    // Every refusal is logged here rather than at each call site: an
    // authorization or limit refusal is a security-relevant event, and the
    // sessions table alone records only that the connection ended.
    log::warn("protocol_refusal", {{"session_id", session_id_},
                                   {"client_id", client_id()},
                                   {"peer", peer_},
                                   {"state", std::string(domain::session_state_name(state()))},
                                   {"code", pb::ErrorCode_Name(code)},
                                   {"message", message},
                                   {"fatal", fatal}});
    pb::ServerFrame frame;
    auto* err = frame.mutable_error();
    err->set_code(code);
    err->set_message(message);
    err->set_fatal(fatal);
    if (fatal) {
        // Recorded before the write so the completion handler knows to close
        // once this frame has gone out. The client always learns why.
        //
        // The FIRST reason wins: whatever actually ended the session is what the
        // audit must record, not whatever happened to be reported last.
        if (close_reason_.empty()) close_reason_ = pb::ErrorCode_Name(code);
        set_state(SessionState::closing);
    }
    auto bytes = messaging::encode_server_frame(frame);
    if (!bytes) {
        close_now("frame_encode_failed");
        return;
    }
    // Bypass the queue bound for the final error: refusing to tell a client why
    // it is being disconnected because its queue is full is the one case where
    // the bound would defeat its own purpose.
    queued_bytes_ += bytes->size();
    queue_.push_back(Pending{std::move(*bytes), false, 0, {}, {}, 0, false});
    write_next();
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

void Session::arm_timer() {
    if (closed_.load(std::memory_order_acquire)) return;
    auto self = shared_from_this();
    timer_.expires_after(kTimerTick);
    timer_.async_wait(asio::bind_executor(strand_, [self](beast::error_code ec) {
        if (ec) return;
        self->tick();
    }));
}

void Session::tick() {
    if (closed_.load(std::memory_order_acquire)) return;
    const int64_t now = now_ms();
    const auto st = state();

    // Handshake deadlines. A connection that opens a socket and says nothing
    // costs a file descriptor and a session slot; it does not get to keep them.
    if (st == SessionState::connected || st == SessionState::hello_sent) {
        if (now - connected_at_ms_ > static_cast<int64_t>(deps_.auth.handshake_timeout_sec) * 1000) {
            log::info("handshake_timeout", {{"session_id", session_id_}, {"peer", peer_},
                                            {"state", std::string(domain::session_state_name(st))}});
            send_error(pb::ERR_TIMEOUT, "handshake not completed in time", true);
            arm_timer();
            return;
        }
    }
    if (st == SessionState::authenticated && authenticated_at_ms_ > 0 &&
        now - authenticated_at_ms_ > static_cast<int64_t>(deps_.ws.resume_timeout_sec) * 1000) {
        log::info("resume_timeout", {{"session_id", session_id_}, {"client_id", client_id()}});
        send_error(pb::ERR_TIMEOUT, "RESUME not sent in time", true);
        arm_timer();
        return;
    }

    if (st == SessionState::authenticated || st == SessionState::resuming ||
        st == SessionState::ready) {
        if (now - last_heartbeat_sent_ms_ >=
            static_cast<int64_t>(deps_.ws.heartbeat_interval_sec) * 1000) {
            heartbeat_nonce_ = domain::random_bytes(kHeartbeatNonceBytes);
            pb::ServerFrame frame;
            auto* hb = frame.mutable_heartbeat();
            hb->set_nonce(heartbeat_nonce_);
            hb->set_server_time_ms(now);
            send_control(frame);
            last_heartbeat_sent_ms_ = now;
        }
        if (last_heartbeat_ack_ms_ > 0 &&
            now - last_heartbeat_ack_ms_ >
                static_cast<int64_t>(deps_.ws.heartbeat_timeout_sec) * 1000) {
            log::warn("heartbeat_timeout", {{"session_id", session_id_},
                                            {"client_id", client_id()},
                                            {"silent_ms", now - last_heartbeat_ack_ms_}});
            send_error(pb::ERR_HEARTBEAT_TIMEOUT, "no heartbeat acknowledgement", true);
            arm_timer();
            return;
        }
        // A client that receives but never acknowledges is not a client this
        // endpoint can reason about: its cursor never advances, so the same
        // signals would be redelivered forever. It is disconnected and the
        // journal redelivers on the next RESUME.
        if (oldest_unacked_ms_ > 0 &&
            now - oldest_unacked_ms_ > static_cast<int64_t>(deps_.ws.ack_timeout_sec) * 1000) {
            log::warn("ack_timeout", {{"session_id", session_id_},
                                      {"client_id", client_id()},
                                      {"unacked", in_flight_.size()},
                                      {"oldest_age_ms", now - oldest_unacked_ms_}});
            send_error(pb::ERR_TIMEOUT, "acknowledgement timeout", true);
            arm_timer();
            return;
        }
    }
    arm_timer();
}

void Session::requeue_outstanding(const std::string& reason) {
    std::vector<std::string> ids = std::move(undelivered_);
    undelivered_.clear();
    ids.reserve(ids.size() + in_flight_.size() + queue_.size());
    for (const auto& [msg_id, rec] : in_flight_) ids.push_back(rec.signal_id);
    for (const auto& p : queue_) {
        if (p.is_signal) ids.push_back(p.signal_id);
    }
    if (ids.empty()) return;
    JournalOp op;
    op.kind = JournalOp::Kind::requeue;
    op.signal_ids = std::move(ids);
    op.client_id = client_id();
    op.session_id = session_id_;
    op.text = reason;
    op.at_ms = now_ms();
    deps_.submit(std::move(op));
}

void Session::terminate(pb::ErrorCode code, const std::string& reason) {
    auto self = shared_from_this();
    asio::post(strand_, [self, code, reason] {
        if (self->closed_.load(std::memory_order_acquire)) return;
        self->send_error(code, reason, true);
    });
}

void Session::close_now(const std::string& reason) {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;
    set_state(SessionState::closed);
    beast::error_code ignored;
    timer_.cancel();
    // Anything this session was holding goes back to the journal so the next
    // session picks it up through RESUME. This is the crash-equivalent path
    // that runs on every ordinary disconnect too.
    requeue_outstanding(reason);

    JournalOp op;
    op.kind = JournalOp::Kind::close_session;
    op.session_id = session_id_;
    op.text = reason;
    op.aux = delivered_count_;
    op.aux2 = acked_count_;
    op.at_ms = now_ms();
    deps_.submit(std::move(op));

    if (deps_.detach) deps_.detach(session_id_);
    log::info("client_disconnected", {{"session_id", session_id_},
                                      {"client_id", client_id()},
                                      {"peer", peer_},
                                      {"reason", reason},
                                      {"delivered", delivered_count_},
                                      {"acked", acked_count_},
                                      {"replayed", replayed_count_}});
    (void)ignored;
    // Beast forbids async_close while a write is outstanding. Issuing one here
    // would be a concurrent operation on the same stream, which is undefined
    // rather than merely untidy — so the close waits for the write completion,
    // which is guaranteed to arrive because the socket is either drained or
    // errored.
    if (writing_) {
        close_pending_ = true;
        return;
    }
    finish_close();
}

void Session::finish_close() {
    close_pending_ = false;
    ws_.async_close(websocket::close_code::normal,
                    asio::bind_executor(strand_, [self = shared_from_this()](beast::error_code) {
                        beast::error_code e;
                        beast::get_lowest_layer(self->ws_).socket().shutdown(
                            tcp::socket::shutdown_both, e);
                    }));
}

} // namespace altsignalgateway::components
