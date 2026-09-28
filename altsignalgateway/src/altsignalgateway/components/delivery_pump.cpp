#include "altsignalgateway/components/delivery_pump.hpp"

#include "altsignalgateway/core/codec.hpp"
#include "altsignalgateway/core/hash.hpp"
#include "altsignalgateway/core/logger.hpp"
#include "altsignalgateway/core/shutdown.hpp"
#include "altsignalgateway/core/time.hpp"
#include "altsignalgateway/domain/canonical.hpp"
#include "altsignalgateway/domain/protocol.hpp"

#include <algorithm>

namespace altsignalgateway::components {

namespace pb = altsignalgateway::v1;

namespace {
// How long the pump sleeps when it has nothing queued. It is woken explicitly
// by the ingest loop, by an acknowledgement and by a resume, so this interval
// governs only the paths that have no wakeup: noticing a session that became
// READY, and driving the expiry sweep and the retention pass on schedule. Short
// enough that a signal is never delayed by it in practice, long enough that an
// idle endpoint costs no measurable CPU.
constexpr auto kIdleTick = std::chrono::milliseconds(250);
} // namespace

DeliveryPump::DeliveryPump(const config::RuntimeConfig& cfg,
                           std::shared_ptr<data::JournalStore> store, SessionRegistry& registry,
                           std::string gateway_version)
    : cfg_(cfg), store_(std::move(store)), registry_(registry),
      gateway_version_(std::move(gateway_version)),
      max_signal_age_ms_(static_cast<int64_t>(cfg.ingest.max_signal_age_sec) * 1000) {}

DeliveryPump::~DeliveryPump() { stop(); }

void DeliveryPump::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] {
        log::set_thread_component("delivery");
        run();
    });
}

void DeliveryPump::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DeliveryPump::submit(JournalOp op) {
    {
        std::lock_guard lock(mu_);
        ops_.push_back(std::move(op));
    }
    cv_.notify_one();
}

void DeliveryPump::wake() { cv_.notify_one(); }

void DeliveryPump::query_cursor(std::string client_id,
                                std::function<void(int64_t, int64_t)> reply) {
    JournalOp op;
    op.kind = JournalOp::Kind::cursor;
    op.client_id = std::move(client_id);
    op.reply = std::move(reply);
    submit(std::move(op));
}

void DeliveryPump::query_backlog(int64_t from, std::function<void(int64_t, int64_t)> reply) {
    JournalOp op;
    op.kind = JournalOp::Kind::backlog;
    op.sequence = from;
    op.reply = std::move(reply);
    submit(std::move(op));
}

void DeliveryPump::run() {
    auto& shutdown = ShutdownToken::instance();
    last_tick_ms_.store(timeutil::now_ms(), std::memory_order_release);
    if (auto head = store_->head_sequence(); head) {
        head_.store(*head, std::memory_order_release);
    }
    while (running_.load(std::memory_order_acquire) && !shutdown.stop_requested()) {
        {
            std::unique_lock lock(mu_);
            if (ops_.empty()) {
                // A short wait, not a long one: the tick also drives the expiry
                // sweep and the retention pass, and it is what notices a
                // session that became READY while the pump was idle.
                cv_.wait_for(lock, kIdleTick);
            }
        }
        drain_ops();
        pump_sessions();
        maintenance();
        // Published AFTER the work, so a thread that is stuck inside any of the
        // three stops advancing it.
        last_tick_ms_.store(timeutil::now_ms(), std::memory_order_release);
    }
    // Apply whatever was queued during the stop so a graceful shutdown does not
    // lose an acknowledgement that already reached the process.
    drain_ops();
    log::info("delivery_pump_stopped", {});
}

void DeliveryPump::drain_ops() {
    for (;;) {
        JournalOp op;
        {
            std::lock_guard lock(mu_);
            if (ops_.empty()) return;
            op = std::move(ops_.front());
            ops_.pop_front();
        }
        apply(op);
    }
}

void DeliveryPump::apply(JournalOp& op) {
    const auto note_failure = [&](const Error& e, const char* what) {
        healthy_.store(false, std::memory_order_release);
        last_error_at_ms_.store(timeutil::now_ms(), std::memory_order_release);
        log::error("journal_write_failed",
                   {{"operation", what}, {"signal_id", op.signal_id}, {"message", e.message}});
    };

    switch (op.kind) {
        case JournalOp::Kind::sent: {
            auto r = store_->mark_sent(op.signal_id, op.client_id, op.session_id, op.attempt,
                                       op.replay, op.at_ms);
            if (!r) note_failure(r.error(), "mark_sent");
            else healthy_.store(true, std::memory_order_release);
            break;
        }
        case JournalOp::Kind::acked: {
            auto a = store_->mark_acked(op.signal_id, op.client_id, op.session_id, op.at_ms);
            if (!a) note_failure(a.error(), "mark_acked");
            else healthy_.store(true, std::memory_order_release);
            break;
        }
        case JournalOp::Kind::cursor_advance: {
            // Submitted separately from the acknowledgement, and always AFTER
            // it in this FIFO: if the process dies between the two the signal
            // reads as ACKED while the cursor still sits below it, so the client
            // is offered it once more on RESUME. That is at-least-once, which is
            // the guarantee. The reverse order would move a cursor past a signal
            // the journal never recorded as delivered — a silent loss.
            auto c = store_->advance_cursor(op.client_id, op.sequence, op.aux, op.at_ms);
            if (!c) note_failure(c.error(), "advance_cursor");
            else healthy_.store(true, std::memory_order_release);
            break;
        }
        case JournalOp::Kind::requeue: {
            auto r = store_->requeue(op.signal_ids, op.client_id, op.session_id, op.text, op.at_ms);
            if (!r) note_failure(r.error(), "requeue");
            break;
        }
        case JournalOp::Kind::open_session: {
            auto r = store_->open_session(op.session_id, op.client_id, op.key_id, op.peer,
                                          op.protocol_version, op.text, op.at_ms);
            if (!r) note_failure(r.error(), "open_session");
            auto t = store_->touch_cursor(op.client_id, op.protocol_version, op.text,
                                          op.session_id, op.at_ms);
            if (!t) note_failure(t.error(), "touch_cursor");
            break;
        }
        case JournalOp::Kind::close_session: {
            auto r = store_->close_session(op.session_id, op.text, op.aux, op.aux2, op.at_ms);
            if (!r) note_failure(r.error(), "close_session");
            replayed_.erase(op.session_id);
            break;
        }
        case JournalOp::Kind::resume_noted: {
            replayed_[op.session_id] = 0;
            auto r = store_->note_session_resume(op.session_id, op.text, op.aux, op.aux2);
            if (!r) note_failure(r.error(), "note_session_resume");
            break;
        }
        case JournalOp::Kind::cursor: {
            auto cursor = store_->load_cursor(op.client_id);
            auto head = store_->head_sequence();
            if (!cursor || !head) {
                // The session is waiting on this answer and must not be left
                // hanging; it is closed by its own handshake timeout, and the
                // failure is visible here.
                note_failure(cursor ? head.error() : cursor.error(), "load_cursor");
                break;
            }
            head_.store(*head, std::memory_order_release);
            if (op.reply) op.reply(cursor->last_acked_sequence, *head);
            break;
        }
        case JournalOp::Kind::backlog: {
            auto backlog = store_->backlog_above(op.sequence, timeutil::now_ms());
            if (!backlog) {
                note_failure(backlog.error(), "backlog");
                // The session is waiting on this answer; leaving it silent
                // would hang it until the resume timeout. Answering "nothing
                // owed" would be a lie, so the session is told the truth by
                // being closed with an internal error.
                break;
            }
            if (op.reply) op.reply(backlog->deliverable, backlog->expired);
            break;
        }
    }
}

bool DeliveryPump::prepare(const data::JournalRecord& rec, bool replay, OutboundSignal* out) {
    // The integrity check that closes the loop between what was stored and what
    // is about to be sent — taken over the STORED BYTES, not over a
    // re-serialization of them.
    //
    // Parsing the payload and letting protobuf write it out again would be a
    // different byte string whenever the running build's schema differs from
    // the one that wrote the row: protobuf preserves unknown fields but re-emits
    // them after the known ones. A rollback to an older build would then mark
    // every newer-schema signal FAILED — permanently, since FAILED is terminal —
    // for a payload that is completely intact. Hashing the bytes themselves
    // detects the thing this check is actually for, which is storage corruption.
    const std::string recomputed = sha256_hex(rec.canonical_payload);
    if (recomputed != rec.payload_sha256) {
        log::critical("payload_hash_mismatch", {{"signal_id", rec.signal_id},
                                                {"sequence", rec.sequence},
                                                {"journalled_sha256", rec.payload_sha256},
                                                {"recomputed_sha256", recomputed},
                                                {"payload_bytes", rec.canonical_payload.size()}});
        // The safest thing a boundary can do with a trade it cannot vouch for is
        // refuse to deliver it. The original upstream bytes are still in the
        // row, so the trade remains recoverable by hand.
        auto r = store_->mark_failed(rec.signal_id,
                                     "payload hash mismatch: journalled " + rec.payload_sha256 +
                                         ", recomputed " + recomputed,
                                     timeutil::now_ms());
        if (!r) {
            log::error("mark_failed_failed",
                       {{"signal_id", rec.signal_id}, {"message", r.error().message}});
        }
        return false;
    }

    domain::EnvelopeSource src;
    src.schema_version = static_cast<uint32_t>(rec.schema_version);
    src.message_id = rec.message_id;
    src.signal_id = rec.signal_id;
    src.payload_sha256 = rec.payload_sha256;
    src.sequence = rec.sequence;
    src.created_at_ms = rec.created_at_ms;
    src.expires_at_ms = rec.expires_at_ms;
    src.journaled_at_ms = rec.journaled_at_ms;
    src.producer_version = gateway_version_;

    const int32_t attempt = rec.delivery_attempts + 1;
    const auto envelope = domain::build_envelope(src, replay, static_cast<uint32_t>(attempt),
                                                 timeutil::now_ms());
    // Spliced, not re-serialized: the bytes on the wire are the bytes in the
    // journal, which are the bytes the hash above covers.
    auto bytes = messaging::encode_signal_frame(envelope, rec.canonical_payload);
    if (!bytes) {
        log::error("signal_frame_encode_failed",
                   {{"signal_id", rec.signal_id}, {"message", bytes.error().message}});
        return false;
    }

    out->sequence = rec.sequence;
    out->signal_id = rec.signal_id;
    out->message_id = rec.message_id;
    out->frame = std::move(*bytes);
    out->attempt = attempt;
    out->replay = replay;
    return true;
}

int64_t DeliveryPump::current_head() {
    // Read from the sequence allocator rather than from the cached value: this
    // number is transmitted to the client as the journal head, and a cached one
    // could be up to a maintenance interval stale. It is read once per session
    // per replay completion, not per message.
    if (auto head = store_->head_sequence(); head) {
        head_.store(*head, std::memory_order_release);
        return *head;
    }
    return head_.load(std::memory_order_acquire);
}

void DeliveryPump::pump_sessions() {
    const auto sessions = registry_.snapshot();
    if (sessions.empty()) return;
    const int64_t now = timeutil::now_ms();

    for (const auto& session : sessions) {
        if (!domain::can_receive_signals(session->state())) continue;
        const int32_t credit = session->delivery_credit();
        if (credit <= 0) continue;

        const int64_t cursor = session->scan_cursor();
        const int32_t limit = std::min(credit, cfg_.journal.delivery_batch_size);
        auto window = store_->fetch_window(cursor, limit);
        if (!window) {
            healthy_.store(false, std::memory_order_release);
            last_error_at_ms_.store(now, std::memory_order_release);
            log::error("journal_read_failed", {{"session_id", session->session_id()},
                                               {"message", window.error().message}});
            continue;
        }
        healthy_.store(true, std::memory_order_release);

        if (window->empty()) {
            // Nothing deliverable above the cursor. The replay window, if any,
            // is complete — and the client is told the REAL journal head rather
            // than the position its own scan happened to stop at, because a
            // window can be empty simply because everything in it had expired.
            if (session->replaying()) {
                session->finish_replay(cursor, current_head(), replayed_[session->session_id()]);
            }
            continue;
        }

        const bool replay = session->replaying();
        std::vector<OutboundSignal> batch;
        std::vector<std::string> queued_ids;
        batch.reserve(window->size());
        queued_ids.reserve(window->size());
        int64_t scanned_to = cursor;
        int64_t skipped = 0;
        for (const auto& rec : *window) {
            scanned_to = rec.sequence;
            // A signal whose validity window has closed is never offered as a
            // live trade. It stays in the journal for audit and the sweep moves
            // it to EXPIRED shortly; skipping it here is what makes the
            // guarantee hold in the gap between the two.
            if (rec.expires_at_ms <= now ||
                (max_signal_age_ms_ > 0 && rec.created_at_ms < now - max_signal_age_ms_)) {
                ++skipped;
                continue;
            }
            OutboundSignal out;
            if (!prepare(rec, replay, &out)) continue;   // FAILED, already recorded
            queued_ids.push_back(rec.signal_id);
            batch.push_back(std::move(out));
        }

        // One transaction for the whole window. If it fails nothing is handed
        // to the session and the scan cursor does not move, so the same window
        // is retried on the next pass rather than being silently skipped.
        if (!queued_ids.empty()) {
            auto q = store_->mark_queued(queued_ids, session->client_id(),
                                         session->session_id(), now);
            if (!q) {
                log::error("mark_queued_failed", {{"session_id", session->session_id()},
                                                  {"count", queued_ids.size()},
                                                  {"message", q.error().message}});
                healthy_.store(false, std::memory_order_release);
                last_error_at_ms_.store(now, std::memory_order_release);
                continue;
            }
        }

        if (skipped > 0) {
            log::debug("expired_signals_skipped", {{"session_id", session->session_id()},
                                                   {"count", skipped},
                                                   {"through_sequence", scanned_to}});
        }
        // `scanned_to` is the highest sequence the query ACTUALLY returned, and
        // the session is allowed to move a client's durable cursor over
        // everything below it that was not offered. That is only sound because
        // sequences become visible in allocation order: the allocator's row lock
        // is held to commit, and there is exactly one ingest writer (enforced by
        // the journal's advisory lock). A row below `scanned_to` therefore
        // cannot still be uncommitted while a row above it is visible, so the
        // window can never have a hole that a later commit fills in.
        session->set_scan_cursor(scanned_to);
        if (replay) replayed_[session->session_id()] += static_cast<int64_t>(batch.size());
        // Always handed over, even when the batch is empty: `scanned_to` is what
        // lets the client's durable cursor move across a run of expired signals
        // it was never offered.
        session->deliver(std::move(batch), scanned_to);

        if (replay && scanned_to >= session->resume_target()) {
            session->finish_replay(scanned_to, current_head(),
                                   replayed_[session->session_id()]);
        }
    }
}

void DeliveryPump::maintenance() {
    const int64_t now = timeutil::now_ms();
    if (now - last_sweep_ms_ >=
        static_cast<int64_t>(cfg_.journal.expiry_sweep_interval_sec) * 1000) {
        last_sweep_ms_ = now;
        auto swept = store_->sweep_expired(now, max_signal_age_ms_);
        if (!swept) {
            log::error("expiry_sweep_failed", {{"message", swept.error().message}});
        } else if (*swept > 0) {
            log::info("signals_expired", {{"count", *swept}});
        }
        if (auto head = store_->head_sequence(); head) {
            head_.store(*head, std::memory_order_release);
        }
    }
    if (now - last_retention_ms_ >=
        static_cast<int64_t>(cfg_.journal.retention_interval_sec) * 1000) {
        // Skipped on the very first pass: a service that has just started has
        // nothing to expire, and running a full delete sweep during startup
        // competes with the recovery reads for the same connections.
        if (last_retention_ms_ != 0) {
            auto deleted = store_->retention_pass();
            if (!deleted) {
                log::error("retention_failed", {{"message", deleted.error().message}});
            } else if (*deleted > 0) {
                log::info("retention_pass", {{"rows_deleted", *deleted}});
            }
        }
        last_retention_ms_ = now;
    }
}

} // namespace altsignalgateway::components
