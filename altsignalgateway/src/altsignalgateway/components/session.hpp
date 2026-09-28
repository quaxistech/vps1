#pragma once

#include "altsignalgateway/components/query_service.hpp"
#include "altsignalgateway/core/config.hpp"
#include "altsignalgateway/domain/auth.hpp"
#include "altsignalgateway/domain/protocol.hpp"
#include "gateway.pb.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace altsignalgateway::components {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

// One journalled signal, prepared for transmission.
//
// The frame bytes are built by the delivery pump, off the I/O threads: protobuf
// serialization of a few kilobytes is cheap but it is still CPU, and an event
// loop that also serializes is an event loop whose latency depends on payload
// size.
struct OutboundSignal {
    int64_t sequence = 0;
    std::string signal_id;
    std::string message_id;
    std::string frame;          // serialized ServerFrame carrying the SignalMessage
    int32_t attempt = 0;
    bool replay = false;
};

// A durable-write request handed back to the delivery pump.
//
// Sessions never touch PostgreSQL. Every journal mutation a session causes is
// queued as one of these and applied by the pump's single database thread, in
// FIFO order — which is also what keeps the delivery state machine's ordering
// (SENT before ACKED) intact without a lock spanning the socket.
struct JournalOp {
    enum class Kind {
        sent, acked, cursor_advance, requeue, open_session, close_session, resume_noted,
        cursor, backlog
    };
    Kind kind = Kind::sent;
    std::string signal_id;
    std::vector<std::string> signal_ids;
    std::string client_id;
    std::string session_id;
    std::string key_id;
    std::string peer;
    std::string text;             // reason / resume mode / client version
    int64_t sequence = 0;
    int64_t aux = 0;              // resume_from / delivered
    int64_t aux2 = 0;             // replayed / acked
    int32_t attempt = 0;
    int32_t protocol_version = 0;
    bool replay = false;
    int64_t at_ms = 0;
    // Read operations answer through this, on the pump thread. Callers post the
    // answer back onto their own executor.
    //   cursor  -> (last_acked_sequence, journal_head)
    //   backlog -> (deliverable_count, expired_count)
    std::function<void(int64_t, int64_t)> reply;
};

// Everything a session needs from the rest of the service, injected so the
// session itself has no knowledge of the store, the registry or the pump.
struct SessionDeps {
    config::WebSocketConfig ws;
    config::AuthConfig auth;
    config::QueryConfig query;
    // Hand a query to the query thread. Returns false when that thread's queue
    // is full; the session then answers ERR_QUERY_LIMIT itself.
    std::function<bool(QueryJob)> run_query;
    const domain::ClientRegistry* registry = nullptr;
    std::string server_version;
    // Submit a durable write. Never blocks.
    std::function<void(JournalOp)> submit;
    // Look up a client's durable cursor and the journal head. Runs on the
    // pump's database thread; the completion is posted back to the session.
    std::function<void(std::string client_id, std::function<void(int64_t cursor,
                                                                 int64_t head)>)> load_cursor;
    // Ask what is still owed above a sequence: (deliverable, expired).
    std::function<void(int64_t from, std::function<void(int64_t, int64_t)>)> query_backlog;
    // Applied once the client identity is known: returns the sessions the
    // duplicate-session policy says must go.
    std::function<void(const std::string& client_id, const std::string& keep_session_id,
                       int32_t max_sessions)> enforce_client_limit;
    std::function<void(const std::string& peer)> note_auth_success;
    // Register / unregister with the session registry. Returns false when the
    // endpoint is at max_clients.
    std::function<bool(std::shared_ptr<class Session>)> attach;
    std::function<void(const std::string& session_id)> detach;
    // Record an authentication failure for the peer address, and ask whether
    // that peer is currently locked out.
    std::function<bool(const std::string& peer)> is_locked_out;
    std::function<void(const std::string& peer)> note_auth_failure;
    // Ask the pump to look at this session now rather than at the next tick.
    std::function<void()> wake_pump;
};

// One authenticated (or authenticating) WebSocket connection.
//
// Threading: every member is touched either on the session's strand or through
// an atomic. The delivery pump calls only the handful of methods documented as
// thread-safe below; everything else is strand-confined.
class Session : public std::enable_shared_from_this<Session> {
public:
    using Stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    Session(tcp::socket&& socket, ssl::context& ssl_ctx, SessionDeps deps);
    ~Session();

    // Start the TLS handshake. Everything after it is driven by completions.
    void run();

    // ---- thread-safe surface used by the delivery pump ----------------------

    [[nodiscard]] std::string session_id() const { return session_id_; }
    [[nodiscard]] std::string client_id() const;
    [[nodiscard]] domain::SessionState state() const {
        return state_.load(std::memory_order_acquire);
    }
    // The highest journal sequence this session has already examined. Written
    // only by the pump thread once the session is visible to it.
    [[nodiscard]] int64_t scan_cursor() const { return scan_cursor_.load(std::memory_order_acquire); }
    void set_scan_cursor(int64_t s) { scan_cursor_.store(s, std::memory_order_release); }
    [[nodiscard]] int64_t resume_target() const { return resume_target_.load(std::memory_order_acquire); }
    [[nodiscard]] bool replaying() const { return replaying_.load(std::memory_order_acquire); }

    // How many more messages this session will accept before its bound is hit.
    // The pump asks BEFORE reading from the journal, so the journal read itself
    // is bounded by what the client can actually take.
    [[nodiscard]] int32_t delivery_credit() const;

    // Hand a prepared batch to the session, together with the highest sequence
    // the pump EXAMINED while producing it.
    //
    // `examined_to` is not cosmetic. The durable client cursor may only advance
    // over sequences that were either acknowledged or never offered at all
    // (expired, or permanently undeliverable), and only the pump knows which
    // sequences it looked at and chose not to send. Handing the batch and the
    // examined watermark together, on one strand, is what makes that decision
    // race-free. An empty batch with a moved watermark is a normal call: it
    // means everything in that window had expired.
    void deliver(std::vector<OutboundSignal> batch, int64_t examined_to);

    // Called by the pump when the replay window has been fully handed over.
    // `last_sequence` is where the replay stopped; `journal_head` is the real
    // head of the journal, which is NOT the same number whenever the tail of
    // the replay window had expired.
    void finish_replay(int64_t last_sequence, int64_t journal_head, int64_t delivered);

    // Close politely with a protocol reason. Safe from any thread.
    void terminate(altsignalgateway::v1::ErrorCode code, const std::string& reason);

private:
    // --- handshake ----------------------------------------------------------
    void on_tls_handshake(beast::error_code ec);
    void on_ws_accept(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes);
    void handle_frame(const altsignalgateway::v1::ClientFrame& frame);
    void handle_hello(const altsignalgateway::v1::Hello& hello);
    void handle_auth(const altsignalgateway::v1::Auth& auth);
    void handle_resume(const altsignalgateway::v1::Resume& resume);
    void handle_ack(const altsignalgateway::v1::Ack& ack);
    void handle_heartbeat_ack(const altsignalgateway::v1::HeartbeatAck& hb);
    void handle_query(const altsignalgateway::v1::QueryRequest& query);
    void send_query_error(const std::string& request_id, altsignalgateway::v1::ErrorCode code,
                          const std::string& message);

    // --- writing ------------------------------------------------------------
    void enqueue(std::string frame, bool is_signal, int64_t sequence,
                 const std::string& signal_id, const std::string& message_id, int32_t attempt,
                 bool replay);
    void write_next();
    void on_write(beast::error_code ec, std::size_t bytes);
    void send_control(const altsignalgateway::v1::ServerFrame& frame);
    void send_error(altsignalgateway::v1::ErrorCode code, const std::string& message, bool fatal);

    // --- lifecycle ----------------------------------------------------------
    void tick();
    void arm_timer();
    // Recompute the durable cursor: the highest sequence below which this
    // session owes the client nothing. Submits a durable write only when it
    // actually moves.
    void maybe_advance_cursor();
    void close_now(const std::string& reason);
    // Perform the WebSocket close. Beast forbids calling async_close while a
    // write is outstanding, so the close is deferred until the write completes.
    void finish_close();
    void requeue_outstanding(const std::string& reason);
    void set_state(domain::SessionState s) { state_.store(s, std::memory_order_release); }
    [[nodiscard]] int64_t now_ms() const;

    // Query bookkeeping (protocol 2), strand-confined. One query in flight per
    // session; a sliding one-minute window bounds the rate.
    bool query_inflight_ = false;
    std::deque<int64_t> query_times_ms_;

    struct Pending {
        std::string frame;
        bool is_signal = false;
        int64_t sequence = 0;
        std::string signal_id;
        std::string message_id;
        int32_t attempt = 0;
        bool replay = false;
    };
    struct InFlight {
        int64_t sequence = 0;
        std::string signal_id;
        int64_t sent_at_ms = 0;
    };

    Stream ws_;
    // The socket is accepted onto its own strand (see WsServer::accept_next),
    // so the stream's own executor IS that strand: every completion handler and
    // every cross-thread post below is serialized against it without a second
    // strand object wrapping the first.
    asio::any_io_executor strand_;
    asio::steady_timer timer_;
    beast::flat_buffer buffer_;
    SessionDeps deps_;

    const std::string session_id_;
    std::string peer_;

    // Handshake state
    std::string server_nonce_;
    std::string client_nonce_;
    std::string claimed_client_id_;
    std::string client_version_;
    uint32_t client_protocol_version_ = 0;
    const domain::AuthorisedClient* client_ = nullptr;

    mutable std::mutex identity_mu_;
    std::string client_id_;

    std::atomic<domain::SessionState> state_{domain::SessionState::connected};
    std::atomic<int64_t> scan_cursor_{0};
    std::atomic<int64_t> resume_target_{0};
    std::atomic<bool> replaying_{false};
    std::atomic<int32_t> outstanding_{0};   // queued + in flight, bounded
    std::atomic<bool> closed_{false};

    // Strand-confined
    std::deque<Pending> queue_;
    uint64_t queued_bytes_ = 0;
    bool writing_ = false;
    std::map<std::string, InFlight> in_flight_;   // message_id -> record
    // Sequences offered to this client and not yet acknowledged — queued, in
    // flight, or dropped without reaching the socket. The SMALLEST of these is
    // the ceiling on the durable cursor.
    std::set<int64_t> outstanding_seqs_;
    int64_t examined_to_ = 0;         // highest sequence the pump has looked at
    int64_t cursor_watermark_ = 0;    // last value submitted for this client
    int64_t acks_since_advance_ = 0;  // acknowledgements folded into the next advance
    // RESUME_LIVE_ONLY starts this session at the journal head. That is a
    // decision about THIS session, not a durable abandonment of the backlog, so
    // such a session never writes a cursor: a later reconnect asking to resume
    // from a sequence still gets everything it was owed.
    bool persist_cursor_ = true;
    int64_t oldest_unacked_ms_ = 0;
    int64_t connected_at_ms_ = 0;
    int64_t authenticated_at_ms_ = 0;
    int64_t last_heartbeat_sent_ms_ = 0;
    int64_t last_heartbeat_ack_ms_ = 0;
    std::string heartbeat_nonce_;
    int64_t delivered_count_ = 0;
    int64_t acked_count_ = 0;
    int64_t replayed_count_ = 0;
    std::string close_reason_;
    bool close_pending_ = false;
    // Signals handed to this session that never reached the socket. They are
    // still QUEUED in the journal, so they must be requeued explicitly or they
    // would sit in that state until the next restart.
    std::vector<std::string> undelivered_;
};

} // namespace altsignalgateway::components
