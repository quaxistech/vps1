#pragma once

#include "altsignalgateway/core/config.hpp"
#include "altsignalgateway/core/error.hpp"
#include "altsignalgateway/core/pg.hpp"
#include "altsignalgateway/domain/delivery_state.hpp"
#include "altsignalgateway/domain/enrichment.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace altsignalgateway::data {

// One journal row, as the delivery pump needs it.
struct JournalRecord {
    int64_t sequence = 0;
    int32_t schema_version = 0;      // the payload's shape as journalled
    std::string signal_id;
    std::string message_id;
    std::string canonical_payload;   // serialized altsignalgateway.v1.SignalPayload
    std::string payload_sha256;
    int64_t created_at_ms = 0;
    int64_t expires_at_ms = 0;
    int64_t journaled_at_ms = 0;
    int32_t delivery_attempts = 0;
    domain::DeliveryState delivery_state = domain::DeliveryState::journaled;
    int64_t last_ack_at_ms = 0;
};

// Everything the ingest path writes for one signal.
struct JournalWrite {
    std::string signal_id;
    std::string message_id;
    std::string signal_proposal_id;
    std::string candidate_id;
    std::string evaluation_id;
    int64_t canonical_pair_id = 0;
    std::string exchange;
    std::string symbol;
    std::string side;                // "long" | "short"
    std::string strategy_id;
    std::string strategy_version;
    int32_t schema_version = 0;
    int32_t protocol_version = 0;
    int64_t created_at_ms = 0;
    int64_t expires_at_ms = 0;
    int64_t received_at_ms = 0;
    // Stamped by the ingest path immediately before the transaction, so the
    // provenance block inside the payload and this row agree on one instant.
    int64_t journaled_at_ms = 0;
    std::string source_payload;
    std::string source_payload_sha256;
    std::string source_subject;
    std::string source_stream;
    int64_t source_stream_sequence = 0;
    std::string canonical_payload;
    std::string payload_sha256;
    std::vector<std::string> warnings;
};

// The outcome of a journalling attempt.
enum class JournalOutcome {
    inserted,           // a new signal; sequence was allocated
    duplicate,          // identical payload already journalled; nothing changed
    integrity_conflict, // SAME signal id, DIFFERENT payload — never overwritten
};

struct JournalResult {
    JournalOutcome outcome = JournalOutcome::inserted;
    int64_t sequence = 0;
    int64_t journaled_at_ms = 0;
    std::string existing_payload_sha256;  // set on integrity_conflict
};

struct ClientCursor {
    std::string client_id;
    int64_t last_acked_sequence = 0;
    int32_t protocol_version = 0;
    std::string client_version;
    int64_t last_seen_at_ms = 0;
    int64_t last_ack_at_ms = 0;
    int64_t total_acked = 0;
};

struct JournalStats {
    int64_t head_sequence = 0;
    int64_t total_rows = 0;
    int64_t journaled = 0;
    int64_t queued = 0;
    int64_t sent = 0;
    int64_t acked = 0;
    int64_t expired = 0;
    int64_t failed = 0;
    int64_t last_signal_at_ms = 0;
    int64_t last_ack_at_ms = 0;
};

// The durable journal.
//
// Every method here is the whole of one transaction. Nothing spans two calls,
// because a half-applied journal write is exactly the failure mode this service
// exists to prevent: a signal that was acknowledged to NATS but never made
// durable is a trade the outside world will never be told about, and no later
// retry can recover it.
class JournalStore {
public:
    JournalStore(const config::RuntimeConfig& cfg, std::string instance_id);

    // Acquire the single-writer advisory lock, verify the schema is present and
    // reconcile any delivery state left behind by an unclean stop.
    //
    // The advisory lock is not decoration: two Gateway processes against one
    // journal would keep two independent session registries and two client
    // cursor caches while sharing one sequence, and the resulting delivery
    // history would be impossible to reason about. It is refused at startup
    // rather than diagnosed later.
    Result<void> bootstrap();

    // Confirm this process still holds the single-writer advisory lock, and
    // re-take it if the connection carrying it was lost.
    //
    // The lock is SESSION-scoped, so it dies with its connection — and a
    // PostgreSQL restart, a network blip or an administrative
    // pg_terminate_backend all do exactly that. The first implementation took
    // the lock once at startup and never looked again, so after a database
    // restart the guarantee was silently gone: a second instance could start,
    // run recovery and begin writing. Verified on the deployed system, where
    // pg_locks held nothing at all after a restart.
    //
    // Returns an error when another instance now owns the lock. That is fatal:
    // this process is no longer the writer and must stop rather than share a
    // journal.
    Result<void> verify_writer_lock();

    // Reset every non-terminal in-flight state to JOURNALED. Called once at
    // startup: QUEUED and SENT are claims about a process that no longer
    // exists, and leaving them would strand those signals forever.
    // Returns how many rows were reclaimed.
    Result<int64_t> recover_in_flight();

    // Journal one signal. Allocates the sequence and writes the row in a single
    // transaction, so a crash either leaves nothing or leaves a complete,
    // numbered, hashed record.
    Result<JournalResult> journal(const JournalWrite& w);

    // Read a delivery window strictly above `after_sequence`. EXPIRED and
    // FAILED rows are excluded by the query; rows that have expired but not yet
    // been swept are returned and skipped by the caller, which is what keeps
    // the scan cursor monotone.
    Result<std::vector<JournalRecord>> fetch_window(int64_t after_sequence, int32_t limit);

    // Delivery-state transitions. Each one is guarded by the state machine both
    // here and in the database.
    // Batched: one transaction for a whole delivery window. A per-signal
    // transaction here would put a database round trip between every pair of
    // frames on the wire, which a replay of a few thousand signals turns into
    // minutes of latency for no benefit.
    Result<void> mark_queued(const std::vector<std::string>& signal_ids,
                             const std::string& client_id, const std::string& session_id,
                             int64_t now_ms);
    Result<void> mark_sent(const std::string& signal_id, const std::string& client_id,
                           const std::string& session_id, int32_t attempt, bool replay,
                           int64_t now_ms);
    Result<void> mark_acked(const std::string& signal_id, const std::string& client_id,
                            const std::string& session_id, int64_t now_ms);
    Result<void> requeue(const std::vector<std::string>& signal_ids, const std::string& client_id,
                         const std::string& session_id, const std::string& reason, int64_t now_ms);
    Result<void> mark_failed(const std::string& signal_id, const std::string& reason,
                             int64_t now_ms);

    // Sweep signals that may no longer be offered as live: those whose validity
    // window closed before they were acknowledged, and — when
    // `max_signal_age_ms` is positive — those whose T0 is older than the
    // configured freshness bound. Returns how many rows moved to EXPIRED.
    Result<int64_t> sweep_expired(int64_t now_ms, int64_t max_signal_age_ms);

    // --- client cursors ------------------------------------------------------
    Result<ClientCursor> load_cursor(const std::string& client_id);
    Result<void> touch_cursor(const std::string& client_id, int32_t protocol_version,
                              const std::string& client_version, const std::string& session_id,
                              int64_t now_ms);
    // Advance a client's durable position to a CONTIGUOUS watermark: the
    // highest sequence below which the client is owed nothing at all. It is not
    // the highest acknowledged sequence — see Session::maybe_advance_cursor.
    // `acked_delta` is how many acknowledgements the caller folded into this
    // advance, so total_acked stays a count of messages rather than of writes.
    Result<void> advance_cursor(const std::string& client_id, int64_t watermark,
                                int64_t acked_delta, int64_t now_ms);

    // --- enrichment reads (read-only, other services' schemas) ---------------
    Result<domain::ContextSnapshot> load_context(const std::string& context_id,
                                                 int64_t expected_revision);
    Result<domain::InstrumentResolution> resolve_instrument(int64_t canonical_pair_id,
                                                            int64_t as_of_ms);
    // AltTech's macro-core vector as of `t0_ms`: the newest minute whose
    // bar_open_time and computed_at <= T0, within `max_age_ms`. A method or
    // source correction computed later cannot enrich an earlier signal.
    // The member names come from AltTech's own registry, cached
    // per feature-set version for the life of the process (they are frozen).
    Result<domain::MacroSnapshot> load_macro_asof(int64_t t0_ms, int64_t max_age_ms);

    // --- query path (protocol 2; read-only) ----------------------------------
    // One journalled signal by either identity. Empty optional when absent.
    Result<std::optional<JournalRecord>> load_record(const std::string& signal_id);
    Result<std::optional<JournalRecord>> load_record_by_sequence(int64_t sequence);

    struct HistoryQuery {
        std::string symbol;
        int64_t canonical_pair_id = 0;
        std::string exchange;
        std::string strategy_id;
        std::string side;              // "" | "long" | "short"
        std::string delivery_state;    // "" | JOURNALED | ... | FAILED
        int64_t from_created_at_ms = 0;   // inclusive; 0 = unbounded
        int64_t to_created_at_ms = 0;     // exclusive; 0 = unbounded
        int64_t before_sequence = 0;      // keyset cursor; 0 = from the head
        int32_t page_size = 50;
    };
    struct HistoryPage {
        std::vector<JournalRecord> records;   // newest first
        int64_t next_before_sequence = 0;     // 0 = no further page
        int64_t head_sequence = 0;
    };
    // Keyset-paginated on `sequence` descending. Reads page_size + 1 rows to
    // learn whether a next page exists, never counts the table.
    Result<HistoryPage> history(const HistoryQuery& q);

    struct CandleWindowSpec {
        std::string timeframe;   // "1m" | "5m" | "1h"
        int32_t bars = 0;
    };
    // The extended market context of one signal at T0: per-venue derivatives,
    // and (when `full`) per-venue book and trade-flow state plus the execution
    // venue's candle windows. Each group that cannot be read is named in
    // `absent` with a reason; nothing is substituted.
    Result<domain::ExtendedContext> load_extended(int64_t canonical_pair_id, int64_t t0_ms,
                                                  bool full,
                                                  const std::vector<CandleWindowSpec>& candles);

    // --- audit ---------------------------------------------------------------
    Result<void> record_rejected(const std::string& signal_id, const std::string& subject,
                                 uint64_t stream_sequence, const std::string& code,
                                 const std::string& detail, const std::string& payload,
                                 int64_t now_ms);
    Result<void> record_integrity_violation(const std::string& signal_id,
                                            const std::string& existing_sha256,
                                            const std::string& incoming_sha256,
                                            int64_t existing_sequence, const std::string& subject,
                                            uint64_t stream_sequence, const std::string& payload,
                                            int64_t now_ms);
    Result<void> open_session(const std::string& session_id, const std::string& client_id,
                              const std::string& key_id, const std::string& peer,
                              int32_t protocol_version, const std::string& client_version,
                              int64_t now_ms);
    Result<void> note_session_resume(const std::string& session_id, const std::string& mode,
                                     int64_t resume_from, int64_t replayed);
    Result<void> close_session(const std::string& session_id, const std::string& reason,
                               int64_t delivered, int64_t acked, int64_t now_ms);

    // --- status / maintenance -------------------------------------------------
    Result<JournalStats> stats();
    Result<int64_t> head_sequence();
    // What is still owed above `sequence`, split into what can still be
    // delivered and what expired before anyone could take it. The second number
    // is what a client would otherwise have to infer from a gap in the sequence
    // numbers it receives.
    struct Backlog {
        int64_t deliverable = 0;
        int64_t expired = 0;
    };
    Result<Backlog> backlog_above(int64_t sequence, int64_t now_ms);
    Result<int64_t> retention_pass();
    // Reconcile the retention_policies rows this service's CONFIGURATION owns.
    //
    // The policy table also carries the rationale for each window and the
    // windows of the tables that have no configuration knob, so it stays the
    // thing retention reads. But `journal.retain_days` and
    // `journal.rejected_retain_days` are documented as settings, and a setting
    // that the running service ignores is worse than no setting at all — so
    // they are pushed into the table at every startup.
    Result<void> reconcile_retention_policies();
    Result<void> note_started(int64_t now_ms);

    // Direct pool access for the readiness probe.
    Result<void> ping();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace altsignalgateway::data
