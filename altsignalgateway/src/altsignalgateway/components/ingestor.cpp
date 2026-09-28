#include "altsignalgateway/components/ingestor.hpp"

#include "altsignalgateway/core/codec.hpp"
#include "altsignalgateway/core/hash.hpp"
#include "altsignalgateway/core/logger.hpp"
#include "altsignalgateway/core/shutdown.hpp"
#include "altsignalgateway/core/time.hpp"
#include "altsignalgateway/domain/canonical.hpp"
#include "altsignalgateway/domain/ids.hpp"
#include "altsignalgateway/domain/protocol.hpp"
#include "altsignalgateway/domain/validation.hpp"

#include <optional>
#include <utility>

namespace altsignalgateway::components {

namespace as = altstrategy::v1;
namespace sq = altsignalquality::v1;

namespace {
// How long JetStream should wait before redelivering a message this service
// could not process. The failures that produce a nak are transient by
// construction — the database was unreachable, or an enrichment read failed —
// and 5 s is long enough for a PostgreSQL restart to complete while being far
// inside the consumer's 60 s AckWait, so a redelivery never races the ack
// window it belongs to.
constexpr int64_t kNakDelayMs = 5'000;
// How long to wait for buffered NATS traffic to leave on the way out. Only
// acknowledgements are in that buffer; the journal is already durable, so an
// expiry here costs a redelivery rather than a signal.
constexpr int64_t kDrainTimeoutMs = 2'000;
// Backoff between reconnection attempts. The consumer has a durable cursor, so
// waiting costs nothing but latency, and retrying tightly against a broker that
// is starting up is how a cold boot turns into a log flood.
constexpr auto kReconnectBackoff = std::chrono::seconds(5);
constexpr auto kRebindBackoff = std::chrono::seconds(2);

// Headroom reserved for the transport envelope and the protobuf framing that
// wraps a canonical payload on the wire: the SignalEnvelope is a few hundred
// bytes of ids, timestamps and one hex hash, plus three tag/length prefixes.
// 4 KiB is an order of magnitude above what those actually cost and keeps the
// bound a round, checkable number.
constexpr uint64_t kFrameHeadroomBytes = 4096;
} // namespace

struct Ingestor::Impl {
    config::RuntimeConfig cfg;
    std::shared_ptr<data::JournalStore> store;
    std::string gateway_version;
    std::unique_ptr<messaging::NatsClient> nats;
    std::optional<messaging::PullConsumer> consumer;
};

Ingestor::Ingestor(const config::RuntimeConfig& cfg, std::shared_ptr<data::JournalStore> store,
                   std::string gateway_version)
    : impl_(std::make_shared<Impl>()) {
    impl_->cfg = cfg;
    impl_->store = std::move(store);
    impl_->gateway_version = std::move(gateway_version);
}

Ingestor::~Ingestor() { stop(); }

void Ingestor::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] {
        log::set_thread_component("ingest");
        run();
    });
}

void Ingestor::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void Ingestor::run() {
    auto& shutdown = ShutdownToken::instance();
    const auto& wiring = impl_->cfg.wiring;

    last_tick_ms_.store(timeutil::now_ms(), std::memory_order_release);
    while (running_.load(std::memory_order_acquire) && !shutdown.stop_requested()) {
        last_tick_ms_.store(timeutil::now_ms(), std::memory_order_release);
        if (impl_->nats == nullptr) {
            auto client = messaging::NatsClient::connect(impl_->cfg.common.nats_url,
                                                         "altsignalgateway-ingest");
            if (!client) {
                connected_.store(false, std::memory_order_release);
                log::error("nats_connect_failed", {{"message", client.error().message}});
                shutdown.wait_for(kReconnectBackoff);
                continue;
            }
            impl_->nats = std::move(*client);
            impl_->consumer.reset();
        }
        if (!impl_->consumer.has_value()) {
            auto consumer = impl_->nats->pull_consumer(wiring.final_stream, wiring.final_durable);
            if (!consumer) {
                connected_.store(false, std::memory_order_release);
                log::error("pull_consumer_bind_failed", {{"stream", wiring.final_stream},
                                                         {"durable", wiring.final_durable},
                                                         {"message", consumer.error().message}});
                impl_->nats.reset();
                shutdown.wait_for(kReconnectBackoff);
                continue;
            }
            impl_->consumer.emplace(std::move(*consumer));
            log::info("ingest_consumer_bound",
                      {{"stream", wiring.final_stream}, {"durable", wiring.final_durable}});
        }
        connected_.store(impl_->nats->connected(), std::memory_order_release);

        auto batch = impl_->consumer->fetch(impl_->cfg.ingest.fetch_batch,
                                            impl_->cfg.ingest.fetch_timeout_ms);
        if (!batch) {
            log::warn("fetch_failed", {{"message", batch.error().message}});
            // A fetch failure against a live connection is usually a consumer
            // that was deleted underneath us; rebinding is the recovery.
            impl_->consumer.reset();
            if (!impl_->nats->connected()) impl_->nats.reset();
            shutdown.wait_for(kRebindBackoff);
            continue;
        }
        if (batch->empty()) continue;

        const int64_t before = journalled_.load(std::memory_order_acquire);
        for (auto& msg : *batch) {
            if (shutdown.stop_requested()) {
                // Leave the message unacknowledged. JetStream redelivers it to
                // the next process; the alternative is to half-process a trade
                // during a shutdown.
                (void)msg.nak(0);
                continue;
            }
            const bool ackable = handle(msg);
            if (ackable) {
                if (auto r = msg.ack(); !r) {
                    // The journal already holds the signal, so the redelivery
                    // this causes is a no-op. Logged, never retried in a loop.
                    log::warn("ack_failed", {{"message", r.error().message}});
                }
            } else {
                if (auto r = msg.nak(kNakDelayMs); !r) {
                    log::warn("nak_failed", {{"message", r.error().message}});
                }
            }
        }
        // Wake the delivery pump only when something new actually became
        // deliverable. A batch of duplicates or rejections has nothing to
        // deliver and does not need to interrupt it.
        if (journalled_.load(std::memory_order_acquire) > before && on_journalled_) {
            on_journalled_();
        }
    }

    if (impl_->nats != nullptr) impl_->nats->drain(kDrainTimeoutMs);
    impl_->consumer.reset();
    impl_->nats.reset();
    connected_.store(false, std::memory_order_release);
    log::info("ingest_stopped", {{"journalled", journalled_.load(std::memory_order_acquire)},
                                 {"duplicates", duplicates_.load(std::memory_order_acquire)},
                                 {"rejected", rejected_.load(std::memory_order_acquire)}});
}

bool Ingestor::handle(messaging::Msg& msg) {
    const int64_t received_at = timeutil::now_ms();
    const std::string subject(msg.subject());
    const std::string payload(msg.data());
    const uint64_t stream_seq = msg.stream_sequence();

    const auto refuse = [&](const std::string& signal_id, domain::RejectionCode code,
                            const std::string& detail) {
        rejected_.fetch_add(1, std::memory_order_acq_rel);
        log::error("signal_rejected", {{"subject", subject},
                                       {"signal_id", signal_id},
                                       {"code", std::string(domain::rejection_code_name(code))},
                                       {"detail", detail},
                                       {"stream_sequence", stream_seq}});
        auto r = impl_->store->record_rejected(signal_id, subject, stream_seq,
                                               std::string(domain::rejection_code_name(code)),
                                               detail, payload, received_at);
        if (!r) {
            // The audit write failed, so the message must NOT be terminated:
            // without a record of the rejection there would be no trace of it
            // at all. A redelivery gives the audit another chance.
            log::error("rejection_record_failed", {{"message", r.error().message}});
            return false;
        }
        // A message that fails contract validation will never become valid.
        // Redelivering it forever would wedge the consumer behind it.
        (void)msg.term();
        return true;
    };

    auto envelope = messaging::parse_upstream_envelope(payload);
    if (!envelope) {
        const auto code = envelope.error().code == ErrorCode::validation_error
                              ? domain::RejectionCode::unsupported_schema_version
                              : domain::RejectionCode::malformed_envelope;
        return refuse({}, code, envelope.error().message);
    }
    auto env_check = domain::validate_envelope(*envelope);
    if (!env_check.ok()) {
        if (env_check.code == domain::RejectionCode::not_a_final_signal) {
            // A stream can legitimately carry other payloads. Nothing is wrong;
            // it is simply not this service's message.
            log::debug("non_final_message_skipped", {{"subject", subject}});
            return true;   // the caller acks; nothing to journal
        }
        return refuse({}, env_check.code, env_check.detail);
    }

    const sq::FinalTradeSignal& sig = envelope->final_trade_signal();
    const auto check = domain::validate_final_signal(sig, received_at);
    if (!check.ok()) {
        return refuse(sig.final_signal_id(), check.code, check.detail);
    }

    const as::TradeSignalProposal& p = sig.proposal();
    const sq::SignalQualityEvaluation& q = sig.quality();

    // --- enrichment (point-in-time, read-only) --------------------------------
    auto ctx = impl_->store->load_context(q.context_id(), p.context_revision());
    if (!ctx) {
        log::error("context_load_failed", {{"signal_id", sig.final_signal_id()},
                                           {"context_id", q.context_id()},
                                           {"message", ctx.error().message}});
        return false;   // a database problem is retried, never turned into an absent snapshot
    }
    auto inst = impl_->store->resolve_instrument(p.canonical_pair_id(), p.created_at_ms());
    if (!inst) {
        log::error("instrument_resolution_failed",
                   {{"signal_id", sig.final_signal_id()},
                    {"canonical_pair_id", p.canonical_pair_id()},
                    {"message", inst.error().message}});
        return false;
    }

    // The macro block is enrichment, not identity: a foreign-server outage
    // must not stop trades from being journalled. It is read as of T0 and,
    // when it cannot be, the payload says so -- MACRO_READ_FAILED is a
    // reason, not a substitute value -- and the failure is logged loudly.
    domain::MacroSnapshot macro;
    {
        auto m = impl_->store->load_macro_asof(
            p.created_at_ms(), static_cast<int64_t>(impl_->cfg.query.macro_max_age_sec) * 1000);
        if (m) {
            macro = std::move(*m);
        } else {
            log::error("macro_load_failed", {{"signal_id", sig.final_signal_id()},
                                             {"t0_ms", p.created_at_ms()},
                                             {"message", m.error().message}});
            macro.present = false;
            macro.absence_reason = "MACRO_READ_FAILED";
        }
    }

    // --- canonicalize ---------------------------------------------------------
    domain::IngestMetadata meta;
    meta.source_subject = subject;
    meta.source_stream = impl_->cfg.wiring.final_stream;
    meta.source_stream_sequence = stream_seq;
    meta.received_at_ms = received_at;
    // Stamped before the transaction so the provenance block inside the payload
    // and the journal row agree on one instant.
    meta.journaled_at_ms = timeutil::now_ms();
    meta.message_id = domain::make_message_id(sig.final_signal_id());
    meta.gateway_version = impl_->gateway_version;

    const auto payload_msg = domain::build_payload(sig, payload, *ctx, *inst, macro, meta);
    auto canonical = messaging::canonical_serialize(payload_msg);
    if (!canonical) {
        return refuse(sig.final_signal_id(), domain::RejectionCode::not_a_final_signal,
                      "canonical serialization failed: " + canonical.error().message);
    }

    // A signal that cannot be transmitted must not be journalled: the journal is
    // a promise of delivery, and a payload larger than the message bound this
    // endpoint advertises in AUTH_OK is a promise the endpoint cannot keep. A
    // conforming consumer would refuse the frame and disconnect, and the signal
    // would be redelivered forever.
    //
    // The upstream contract has no length limits of its own — every free-text
    // field on a proposal is unbounded — so this is where the boundary imposes
    // one. It is recorded in rejected_messages with its full bytes, so an
    // upstream that starts producing them is diagnosable rather than merely
    // absent.
    const uint64_t budget = impl_->cfg.websocket.max_message_bytes > kFrameHeadroomBytes
                                ? impl_->cfg.websocket.max_message_bytes - kFrameHeadroomBytes
                                : 0;
    if (canonical->size() > budget) {
        return refuse(sig.final_signal_id(), domain::RejectionCode::payload_too_large,
                      "canonical payload is " + std::to_string(canonical->size()) +
                          " bytes; the transmissible budget is " + std::to_string(budget) +
                          " (websocket.max_message_bytes minus framing headroom)");
    }

    data::JournalWrite w;
    w.signal_id = sig.final_signal_id();
    w.message_id = meta.message_id;
    w.signal_proposal_id = p.signal_proposal_id();
    w.candidate_id = p.candidate_id();
    w.evaluation_id = q.evaluation_id();
    w.canonical_pair_id = p.canonical_pair_id();
    w.exchange = inst->exchange;
    // The executable symbol, not the internal subject token.
    w.symbol = inst->exchange_symbol;
    w.side = p.side() == as::SIDE_LONG ? "long" : "short";
    w.strategy_id = p.strategy_id();
    w.strategy_version = p.strategy_version();
    w.schema_version = static_cast<int32_t>(domain::kSchemaVersion);
    w.protocol_version = static_cast<int32_t>(domain::kProtocolVersion);
    w.created_at_ms = p.created_at_ms();
    w.expires_at_ms = p.valid_until_ms();
    w.received_at_ms = received_at;
    w.journaled_at_ms = meta.journaled_at_ms;
    w.source_payload = payload;
    w.source_payload_sha256 = sha256_hex(payload);
    w.source_subject = subject;
    w.source_stream = impl_->cfg.wiring.final_stream;
    w.source_stream_sequence = static_cast<int64_t>(stream_seq);
    w.canonical_payload = *canonical;
    w.payload_sha256 = sha256_hex(*canonical);
    w.warnings = check.warnings;
    if (!ctx->present) w.warnings.push_back("MARKET_SNAPSHOT_ABSENT:" + ctx->absence_reason);
    if (!inst->present) w.warnings.push_back("INSTRUMENT_UNRESOLVED:" + inst->absence_reason);
    else if (!inst->spec_present) {
        w.warnings.push_back("EXECUTION_SPEC_ABSENT:" + inst->spec_absence_reason);
    }

    auto result = impl_->store->journal(w);
    if (!result) {
        log::error("journal_write_failed", {{"signal_id", w.signal_id},
                                            {"message", result.error().message}});
        return false;   // retried; the ack has not been sent, so nothing is lost
    }

    switch (result->outcome) {
        case data::JournalOutcome::inserted:
            journalled_.fetch_add(1, std::memory_order_acq_rel);
            last_signal_at_ms_.store(result->journaled_at_ms, std::memory_order_release);
            log::info("signal_journalled",
                      {{"signal_id", w.signal_id},
                       {"message_id", w.message_id},
                       {"sequence", result->sequence},
                       {"signal_proposal_id", w.signal_proposal_id},
                       {"symbol", w.symbol},
                       {"side", w.side},
                       {"strategy_id", w.strategy_id},
                       {"payload_sha256", w.payload_sha256},
                       {"payload_bytes", w.canonical_payload.size()},
                       {"expires_in_ms", w.expires_at_ms - received_at},
                       {"market_snapshot", ctx->present},
                       {"execution_spec", inst->spec_present},
                       {"warnings", w.warnings}});
            break;
        case data::JournalOutcome::duplicate:
            duplicates_.fetch_add(1, std::memory_order_acq_rel);
            log::info("signal_already_journalled",
                      {{"signal_id", w.signal_id}, {"sequence", result->sequence}});
            break;
        case data::JournalOutcome::integrity_conflict:
            conflicts_.fetch_add(1, std::memory_order_acq_rel);
            // The one inbound condition that is never resolved by overwriting.
            // One of the two payloads is the trade VPS1 decided on and this
            // service cannot know which; it keeps the first and refuses the
            // second, loudly.
            log::critical("signal_integrity_violation",
                          {{"signal_id", w.signal_id},
                           {"existing_sequence", result->sequence},
                           {"incoming_source_sha256", w.source_payload_sha256},
                           {"subject", subject},
                           {"message",
                            "the same final_signal_id arrived with different upstream bytes; the "
                            "journalled signal is kept unchanged"}});
            {
                auto rec = impl_->store->record_integrity_violation(
                    w.signal_id, result->existing_payload_sha256, w.source_payload_sha256,
                    result->sequence, subject, stream_seq, payload, received_at);
                if (!rec) {
                    log::error("integrity_record_failed", {{"message", rec.error().message}});
                    return false;
                }
            }
            (void)msg.term();
            return true;
    }
    return true;
}

} // namespace altsignalgateway::components
