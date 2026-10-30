#include "altsignalgateway/data/store.hpp"

#include "altsignalgateway/core/hash.hpp"
#include "altsignalgateway/core/logger.hpp"
#include "altsignalgateway/core/shutdown.hpp"
#include "altsignalgateway/core/time.hpp"
#include "altsignalgateway/data/pg_bytea.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace altsignalgateway::data {

namespace {

// A fixed 64-bit key for the single-writer advisory lock. Derived once from the
// service name so it cannot collide with another service's lock on the shared
// database by accident.
constexpr int64_t kWriterLockKey = 0x41'53'47'57'4a'52'4e'4cLL;  // "ASGWJRNL"

std::optional<double> opt_double(const pqxx::field& f) {
    if (f.is_null()) return std::nullopt;
    const double v = f.as<double>();
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

std::optional<int32_t> opt_int32(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return std::nullopt;
    const auto& v = j.at(key);
    if (v.is_number_integer() || v.is_number_unsigned()) return v.get<int32_t>();
    if (v.is_string()) {
        try {
            return static_cast<int32_t>(std::stol(v.get<std::string>()));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<double> opt_double_json(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return std::nullopt;
    const auto& v = j.at(key);
    if (v.is_number()) {
        const double d = v.get<double>();
        return std::isfinite(d) ? std::optional(d) : std::nullopt;
    }
    if (v.is_string()) {
        try {
            const double d = std::stod(v.get<std::string>());
            return std::isfinite(d) ? std::optional(d) : std::nullopt;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// The registry stores tick/lot values as decimal STRINGS. They are carried
// through as strings on purpose (see gateway.proto), so a numeric JSON value is
// rendered back to its shortest exact text rather than being parsed to double.
std::string decimal_string(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return {};
    const auto& v = j.at(key);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return v.dump();
    return {};
}

void collect_named(const nlohmann::json& obj, std::vector<domain::NamedScalar>& out) {
    if (!obj.is_object()) return;
    out.reserve(out.size() + obj.size());
    for (const auto& [k, v] : obj.items()) {
        if (!v.is_number()) continue;
        const double d = v.get<double>();
        if (!std::isfinite(d)) continue;   // absent, never substituted
        out.push_back(domain::NamedScalar{k, d});
    }
    std::sort(out.begin(), out.end(),
              [](const domain::NamedScalar& a, const domain::NamedScalar& b) {
                  return a.name < b.name;
              });
}

domain::RegimeSnapshot parse_regime(const nlohmann::json& obj) {
    domain::RegimeSnapshot r;
    if (!obj.is_object()) return r;
    if (obj.contains("regime") && obj.at("regime").is_string()) {
        r.regime = obj.at("regime").get<std::string>();
    }
    r.confidence = opt_double_json(obj, "confidence");
    r.trend_strength = opt_double_json(obj, "trend_strength");
    r.volatility_regime = opt_double_json(obj, "volatility_regime");
    r.liquidity_quality = opt_double_json(obj, "liquidity_quality");
    return r;
}

std::string text_array_literal(const std::vector<std::string>& values) {
    std::string out = "{";
    bool first = true;
    for (const auto& v : values) {
        if (!first) out += ',';
        first = false;
        out += '"';
        for (char c : v) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        out += '"';
    }
    out += '}';
    return out;
}

} // namespace

namespace {
// One projection for every JournalRecord read, so the column positions the
// parsers below rely on cannot drift between the delivery window and the
// query path.
constexpr const char* kJournalRecordColumns =
    "SELECT sequence, signal_id, message_id, encode(canonical_payload,'hex'),"
    "       payload_sha256, created_at_ms, expires_at_ms, journaled_at_ms,"
    "       delivery_attempts, delivery_state, schema_version, last_ack_at_ms";

JournalRecord parse_record(const pqxx::row& row) {
    JournalRecord rec;
    rec.sequence = row[0].as<int64_t>();
    rec.signal_id = row[1].as<std::string>();
    rec.message_id = row[2].as<std::string>();
    rec.canonical_payload = pgbytea::from_hex_literal("\\x" + row[3].as<std::string>());
    rec.payload_sha256 = row[4].as<std::string>();
    rec.created_at_ms = row[5].as<int64_t>();
    rec.expires_at_ms = row[6].as<int64_t>();
    rec.journaled_at_ms = row[7].as<int64_t>();
    rec.delivery_attempts = row[8].as<int32_t>();
    const auto state_token = row[9].as<std::string>();
    const auto parsed_state = domain::parse_delivery_state(state_token);
    if (!parsed_state) {
        throw std::runtime_error("unknown delivery_state '" + state_token + "' on signal " +
                                 rec.signal_id);
    }
    rec.delivery_state = *parsed_state;
    rec.schema_version = row[10].as<int32_t>();
    rec.last_ack_at_ms = row[11].is_null() ? 0 : row[11].as<int64_t>();
    return rec;
}

std::optional<int64_t> opt_int64(const pqxx::field& f) {
    if (f.is_null()) return std::nullopt;
    return f.as<int64_t>();
}
} // namespace

struct JournalStore::Impl {
    config::RuntimeConfig cfg;
    std::string instance_id;
    persistence::PgPool pool;
    // The advisory lock is session-scoped, so the connection that took it must
    // stay leased for the lifetime of the service. Holding it in a dedicated
    // lease is what keeps the lock alive.
    std::optional<persistence::PgPool::Lease> writer_lock;

    // AltTech's macro-core member names, by feature_set_version_id. The set is
    // frozen upstream (feature_set_versions.frozen), so the first read is the
    // only one this process ever needs.
    std::mutex macro_names_mu;
    std::map<int64_t, std::vector<std::string>> macro_names;

    Impl(const config::RuntimeConfig& c, std::string id)
        : cfg(c), instance_id(std::move(id)),
          pool(c.common.postgres_dsn, static_cast<size_t>(c.journal.postgres_pool_size)) {}
};

JournalStore::JournalStore(const config::RuntimeConfig& cfg, std::string instance_id)
    : impl_(std::make_shared<Impl>(cfg, std::move(instance_id))) {}

Result<void> JournalStore::bootstrap() {
    // A cold boot races PostgreSQL's own startup, and a restart during an
    // outage would otherwise fail immediately, burn a slot of the unit's
    // restart budget, and — once the budget is spent — leave the boundary down
    // after the database has come back. Waiting here instead is bounded by the
    // unit's TimeoutStartSec and by a stop request, so a SIGTERM during startup
    // still returns promptly.
    constexpr int kMaxAttempts = 30;
    std::optional<persistence::PgPool::Lease> lease_holder;
    for (int attempt = 1;; ++attempt) {
        auto attempt_lease = impl_->pool.acquire();
        if (attempt_lease) {
            lease_holder.emplace(std::move(*attempt_lease));
            break;
        }
        if (attempt >= kMaxAttempts || ShutdownToken::instance().stop_requested()) {
            return std::unexpected(attempt_lease.error());
        }
        if (attempt == 1) {
            log::info("waiting_for_database", {{"message", attempt_lease.error().message}});
        }
        ShutdownToken::instance().wait_for(std::chrono::seconds(2));
    }
    auto* lease = &*lease_holder;
    try {
        pqxx::work txn(lease->conn());
        // Fail fast and legibly if db-bootstrap.sh has not been run, rather
        // than producing an undefined-table error on the first signal.
        const auto present =
            txn.query_value<bool>("SELECT to_regclass('altsignalgateway.signal_journal') IS NOT NULL");
        if (!present) {
            txn.abort();
            return fail(ErrorCode::database_error,
                        "altsignalgateway.signal_journal is missing: run db-bootstrap.sh before "
                        "starting the service");
        }
        const auto locked = txn.query_value<bool>(
            "SELECT pg_try_advisory_lock(" + std::to_string(kWriterLockKey) + ")");
        if (!locked) {
            txn.abort();
            return fail(ErrorCode::database_error,
                        "another AltSignalGateway instance already holds the journal writer lock "
                        "on this database; two writers would share one sequence while keeping "
                        "two independent client cursors");
        }
        txn.commit();
    } catch (const std::exception& e) {
        return fail(ErrorCode::database_error, std::string("journal bootstrap failed: ") + e.what());
    }
    // Keep the lease (and therefore the session, and therefore the lock).
    // emplace, not assignment: PgPool::Lease is move-CONSTRUCTIBLE but not
    // move-assignable, because assigning over a live lease would silently
    // return a connection to the pool while someone still held a reference.
    impl_->writer_lock.emplace(std::move(*lease_holder));
    return {};
}

Result<void> JournalStore::verify_writer_lock() {
    // A live session still holds the lock: nothing releases an advisory lock
    // except an explicit unlock (which this service never issues) or the end of
    // the session. So proving the session is alive proves the lock is held —
    // and it takes an actual query, because a connection can look open long
    // after its socket has gone.
    if (impl_->writer_lock.has_value()) {
        try {
            pqxx::nontransaction probe(impl_->writer_lock->conn());
            (void)probe.query_value<int>("SELECT 1");
            return {};
        } catch (const std::exception& e) {
            log::warn("writer_lock_connection_lost",
                      {{"message", e.what()},
                       {"action", "re-acquiring the single-writer advisory lock"}});
            // Discarded, not returned to the pool: it is broken.
            impl_->writer_lock->discard();
            impl_->writer_lock.reset();
        }
    }

    auto lease = impl_->pool.acquire();
    if (!lease) return std::unexpected(lease.error());
    try {
        pqxx::work txn(lease->conn());
        const auto locked = txn.query_value<bool>(
            "SELECT pg_try_advisory_lock(" + std::to_string(kWriterLockKey) + ")");
        if (!locked) {
            txn.abort();
            return fail(ErrorCode::database_error,
                        "the journal writer lock is now held by another instance; this process "
                        "is no longer the writer and must stop");
        }
        txn.commit();
    } catch (const std::exception& e) {
        return fail(ErrorCode::database_error,
                    std::string("could not re-acquire the writer lock: ") + e.what(), true);
    }
    impl_->writer_lock.emplace(std::move(*lease));
    log::info("writer_lock_reacquired", {});
    return {};
}

Result<int64_t> JournalStore::recover_in_flight() {
    int64_t reclaimed = 0;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        const auto res = txn.exec_params(
            "UPDATE altsignalgateway.signal_journal"
            "   SET delivery_state = 'JOURNALED'"
            " WHERE delivery_state IN ('QUEUED','SENT')"
            " RETURNING signal_id, sequence");
        reclaimed = static_cast<int64_t>(res.size());
        for (const auto& row : res) {
            txn.exec_params(
                "INSERT INTO altsignalgateway.delivery_events"
                " (signal_id, sequence, event, detail, occurred_at_ms)"
                " VALUES ($1,$2,'REQUEUED','recovered after restart',$3)",
                row[0].as<std::string>(), row[1].as<int64_t>(), timeutil::now_ms());
        }
    });
    if (!r) return std::unexpected(r.error());
    return reclaimed;
}

Result<JournalResult> JournalStore::journal(const JournalWrite& w) {
    JournalResult out;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        // Idempotency first. The signal id is deterministic in AltStrategy's
        // proposal id, so a redelivery lands on the same row; whether that is
        // harmless or a stack-level defect is decided entirely by the payload
        // hash.
        const auto existing = txn.exec_params(
            "SELECT sequence, payload_sha256, source_payload_sha256, journaled_at_ms"
            "  FROM altsignalgateway.signal_journal WHERE signal_id = $1",
            w.signal_id);
        if (!existing.empty()) {
            const auto seq = existing[0][0].as<int64_t>();
            const auto payload_hash = existing[0][1].as<std::string>();
            const auto source_hash = existing[0][2].as<std::string>();
            out.sequence = seq;
            out.journaled_at_ms = existing[0][3].as<int64_t>();
            out.existing_payload_sha256 = payload_hash;
            // The SOURCE hash is the arbiter, not the canonical one: the
            // canonical projection can legitimately change between builds (a
            // new enrichment field, a registry that has since resolved), while
            // the upstream bytes are the trade itself. Comparing the projection
            // would report an integrity violation every time this service was
            // upgraded.
            out.outcome = (source_hash == w.source_payload_sha256)
                              ? JournalOutcome::duplicate
                              : JournalOutcome::integrity_conflict;
            return;
        }

        // Allocate the sequence INSIDE this transaction. The row lock is held
        // to commit, which is what makes the sequence gapless and makes its
        // visibility order match its allocation order.
        const auto seq = txn.query_value<int64_t>(
            "UPDATE altsignalgateway.sequence_state"
            "   SET last_sequence = last_sequence + 1, updated_at = now()"
            " WHERE singleton RETURNING last_sequence");

        const int64_t journaled_at = w.journaled_at_ms;
        txn.exec_params(
            "INSERT INTO altsignalgateway.signal_journal"
            " (signal_id, message_id, sequence, signal_proposal_id, candidate_id, evaluation_id,"
            "  canonical_pair_id, exchange, symbol, side, strategy_id, strategy_version,"
            "  schema_version, protocol_version, created_at_ms, expires_at_ms, received_at_ms,"
            "  journaled_at_ms, source_payload, source_payload_sha256, source_subject,"
            "  source_stream, source_stream_sequence, canonical_payload, payload_sha256, warnings)"
            " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,"
            "         $19::bytea,$20,$21,$22,$23,$24::bytea,$25,$26::text[])",
            w.signal_id, w.message_id, seq, w.signal_proposal_id, w.candidate_id, w.evaluation_id,
            w.canonical_pair_id, w.exchange, w.symbol, w.side, w.strategy_id, w.strategy_version,
            w.schema_version, w.protocol_version, w.created_at_ms, w.expires_at_ms,
            w.received_at_ms, journaled_at, pgbytea::hex_literal(w.source_payload),
            w.source_payload_sha256, w.source_subject, w.source_stream, w.source_stream_sequence,
            pgbytea::hex_literal(w.canonical_payload), w.payload_sha256,
            text_array_literal(w.warnings));

        txn.exec_params(
            "UPDATE altsignalgateway.service_state"
            "   SET instance_id = $1, last_signal_id = $2, last_signal_at_ms = $3,"
            "       signals_journalled = signals_journalled + 1, updated_at = now()"
            " WHERE singleton",
            impl_->instance_id, w.signal_id, journaled_at);

        out.outcome = JournalOutcome::inserted;
        out.sequence = seq;
        out.journaled_at_ms = journaled_at;
    });
    if (!r) return std::unexpected(r.error());
    return out;
}

Result<std::vector<JournalRecord>> JournalStore::fetch_window(int64_t after_sequence,
                                                              int32_t limit) {
    std::vector<JournalRecord> out;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        const auto res = txn.exec_params(
            std::string(kJournalRecordColumns) +
            "  FROM altsignalgateway.signal_journal"
            " WHERE sequence > $1 AND delivery_state NOT IN ('EXPIRED','FAILED')"
            " ORDER BY sequence LIMIT $2",
            after_sequence, limit);
        out.reserve(res.size());
        // parse_record refuses an unrecognised delivery state rather than
        // defaulting it: quietly reading one as JOURNALED would offer a
        // terminal signal for delivery all over again.
        for (const auto& row : res) out.push_back(parse_record(row));
    });
    if (!r) return std::unexpected(r.error());
    return out;
}

Result<void> JournalStore::mark_queued(const std::vector<std::string>& signal_ids,
                                       const std::string& client_id,
                                       const std::string& session_id, int64_t now_ms) {
    if (signal_ids.empty()) return {};
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        const std::string ids = text_array_literal(signal_ids);
        // The state guard in the WHERE clause makes this a no-op rather than an
        // error when a concurrent session already advanced a row.
        txn.exec_params(
            "UPDATE altsignalgateway.signal_journal"
            "   SET delivery_state = 'QUEUED', last_session_id = $2"
            " WHERE signal_id = ANY($1::text[]) AND delivery_state = 'JOURNALED'",
            ids, session_id);
        txn.exec_params(
            "INSERT INTO altsignalgateway.delivery_events"
            " (signal_id, sequence, client_id, session_id, event, occurred_at_ms)"
            " SELECT signal_id, sequence, $2, $3, 'QUEUED', $4"
            "   FROM altsignalgateway.signal_journal WHERE signal_id = ANY($1::text[])",
            ids, client_id, session_id, now_ms);
    });
}

Result<void> JournalStore::mark_sent(const std::string& signal_id, const std::string& client_id,
                                     const std::string& session_id, int32_t attempt, bool replay,
                                     int64_t now_ms) {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "UPDATE altsignalgateway.signal_journal"
            "   SET delivery_state = 'SENT',"
            "       delivery_attempts = delivery_attempts + 1,"
            "       first_sent_at_ms = COALESCE(first_sent_at_ms, $2),"
            "       last_delivery_at_ms = $2,"
            "       last_session_id = $3"
            " WHERE signal_id = $1 AND delivery_state IN ('QUEUED','SENT')",
            signal_id, now_ms, session_id);
        txn.exec_params(
            "INSERT INTO altsignalgateway.delivery_events"
            " (signal_id, sequence, client_id, session_id, event, attempt, replay, occurred_at_ms)"
            " SELECT signal_id, sequence, $2, $3, $4, $5, $6, $7"
            "   FROM altsignalgateway.signal_journal WHERE signal_id = $1",
            signal_id, client_id, session_id, replay ? "REPLAYED" : "SENT", attempt, replay,
            now_ms);
    });
}

Result<void> JournalStore::mark_acked(const std::string& signal_id, const std::string& client_id,
                                      const std::string& session_id, int64_t now_ms) {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "UPDATE altsignalgateway.signal_journal"
            "   SET delivery_state = 'ACKED', last_ack_at_ms = $2, acked_by_client_id = $3,"
            "       last_session_id = $4"
            " WHERE signal_id = $1 AND delivery_state IN ('SENT','ACKED')",
            signal_id, now_ms, client_id, session_id);
        txn.exec_params(
            "INSERT INTO altsignalgateway.delivery_events"
            " (signal_id, sequence, client_id, session_id, event, occurred_at_ms)"
            " SELECT signal_id, sequence, $2, $3, 'ACKED', $4"
            "   FROM altsignalgateway.signal_journal WHERE signal_id = $1",
            signal_id, client_id, session_id, now_ms);
        txn.exec_params(
            "UPDATE altsignalgateway.service_state SET last_ack_at_ms = $1, updated_at = now()"
            " WHERE singleton",
            now_ms);
    });
}

Result<void> JournalStore::requeue(const std::vector<std::string>& signal_ids,
                                   const std::string& client_id, const std::string& session_id,
                                   const std::string& reason, int64_t now_ms) {
    if (signal_ids.empty()) return {};
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        const std::string ids = text_array_literal(signal_ids);
        // Only in-flight rows are reclaimed. An ACKED row that happens to be in
        // a lost session's queue is already delivered and must stay delivered.
        txn.exec_params(
            "UPDATE altsignalgateway.signal_journal"
            "   SET delivery_state = 'JOURNALED'"
            " WHERE signal_id = ANY($1::text[]) AND delivery_state IN ('QUEUED','SENT')",
            ids);
        txn.exec_params(
            "INSERT INTO altsignalgateway.delivery_events"
            " (signal_id, sequence, client_id, session_id, event, detail, occurred_at_ms)"
            " SELECT signal_id, sequence, $2, $3, 'REQUEUED', $4, $5"
            "   FROM altsignalgateway.signal_journal WHERE signal_id = ANY($1::text[])",
            ids, client_id, session_id, reason, now_ms);
    });
}

Result<void> JournalStore::mark_failed(const std::string& signal_id, const std::string& reason,
                                       int64_t now_ms) {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "UPDATE altsignalgateway.signal_journal"
            "   SET delivery_state = 'FAILED', failure_reason = $2"
            " WHERE signal_id = $1 AND delivery_state IN ('JOURNALED','QUEUED','SENT')",
            signal_id, reason);
        txn.exec_params(
            "INSERT INTO altsignalgateway.delivery_events"
            " (signal_id, sequence, event, detail, occurred_at_ms)"
            " SELECT signal_id, sequence, 'FAILED', $2, $3"
            "   FROM altsignalgateway.signal_journal WHERE signal_id = $1",
            signal_id, reason, now_ms);
    });
}

Result<int64_t> JournalStore::sweep_expired(int64_t now_ms, int64_t max_signal_age_ms) {
    int64_t swept = 0;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        // Two independent reasons a signal may no longer go out as live:
        //   * its own validity window closed (the trade's rule, always applied);
        //   * it is older than the configured freshness bound, which is an
        //     operator's rule ON TOP of the trade's and is disabled at 0.
        // The age cut is expressed against created_at_ms (T0), not against
        // arrival: a signal that sat in a stopped consumer for an hour is an
        // hour old whatever moment this service first saw it.
        // EX-11 (2026-10-30): the state each signal left is part of the
        // record -- never delivered (JOURNALED/QUEUED) is a different truth
        // from sent and never acknowledged (SENT).
        const auto res = txn.exec_params(
            "WITH due AS (SELECT signal_id, delivery_state AS prior_state FROM altsignalgateway.signal_journal"
            "  WHERE delivery_state IN ('JOURNALED','QUEUED','SENT')"
            "    AND (expires_at_ms <= $1 OR ($2 > 0 AND created_at_ms < $1 - $2)) FOR UPDATE)"
            " UPDATE altsignalgateway.signal_journal j"
            "   SET delivery_state = 'EXPIRED'"
            "  FROM due WHERE j.signal_id = due.signal_id"
            " RETURNING j.signal_id, j.sequence, j.expires_at_ms <= $1 AS by_validity, due.prior_state",
            now_ms, max_signal_age_ms);
        swept = static_cast<int64_t>(res.size());
        for (const auto& row : res) {
            const auto prior = row[3].as<std::string>();
            const std::string why = std::string(row[2].as<bool>() ? "validity window closed before acknowledgement"
                                                                  : "older than the configured max_signal_age_sec") +
                                    (prior == "SENT" ? "; sent, never acknowledged" : "; never delivered (" + prior + ")");
            txn.exec_params(
                "INSERT INTO altsignalgateway.delivery_events"
                " (signal_id, sequence, event, detail, occurred_at_ms)"
                " VALUES ($1,$2,'EXPIRED',$3,$4)",
                row[0].as<std::string>(), row[1].as<int64_t>(), why, now_ms);
        }
    });
    if (!r) return std::unexpected(r.error());
    return swept;
}

Result<ClientCursor> JournalStore::load_cursor(const std::string& client_id) {
    ClientCursor c;
    c.client_id = client_id;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        const auto res = txn.exec_params(
            "SELECT last_acked_sequence, protocol_version, client_version, last_seen_at_ms,"
            "       last_ack_at_ms, total_acked"
            "  FROM altsignalgateway.client_cursors WHERE client_id = $1",
            client_id);
        if (res.empty()) return;
        c.last_acked_sequence = res[0][0].as<int64_t>();
        c.protocol_version = res[0][1].as<int32_t>();
        c.client_version = res[0][2].as<std::string>();
        c.last_seen_at_ms = res[0][3].as<int64_t>();
        c.last_ack_at_ms = res[0][4].as<int64_t>();
        c.total_acked = res[0][5].as<int64_t>();
    });
    if (!r) return std::unexpected(r.error());
    return c;
}

Result<void> JournalStore::touch_cursor(const std::string& client_id, int32_t protocol_version,
                                        const std::string& client_version,
                                        const std::string& session_id, int64_t now_ms) {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altsignalgateway.client_cursors"
            " (client_id, protocol_version, client_version, last_seen_at_ms, last_session_id)"
            " VALUES ($1,$2,$3,$4,$5)"
            " ON CONFLICT (client_id) DO UPDATE SET"
            "   protocol_version = EXCLUDED.protocol_version,"
            "   client_version = EXCLUDED.client_version,"
            "   last_seen_at_ms = EXCLUDED.last_seen_at_ms,"
            "   last_session_id = EXCLUDED.last_session_id,"
            "   updated_at = now()",
            client_id, protocol_version, client_version, now_ms, session_id);
    });
}

Result<void> JournalStore::advance_cursor(const std::string& client_id, int64_t watermark,
                                          int64_t acked_delta, int64_t now_ms) {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        // GREATEST is a safety net, not the rule. The caller already computes a
        // contiguous watermark; GREATEST guarantees that a late write from a
        // superseded session — or a second concurrent session for the same
        // client_id, which max_sessions > 1 permits — can never rewind it. A
        // cursor that rewound would re-deliver acknowledged trades.
        txn.exec_params(
            "INSERT INTO altsignalgateway.client_cursors"
            " (client_id, last_acked_sequence, last_ack_at_ms, last_seen_at_ms, total_acked)"
            " VALUES ($1,$2,$3,$3,$4)"
            " ON CONFLICT (client_id) DO UPDATE SET"
            "   last_acked_sequence = GREATEST(altsignalgateway.client_cursors.last_acked_sequence,"
            "                                  EXCLUDED.last_acked_sequence),"
            "   last_ack_at_ms = EXCLUDED.last_ack_at_ms,"
            "   last_seen_at_ms = EXCLUDED.last_seen_at_ms,"
            "   total_acked = altsignalgateway.client_cursors.total_acked + $4,"
            "   updated_at = now()",
            client_id, watermark, now_ms, acked_delta);
    });
}

Result<domain::ContextSnapshot> JournalStore::load_context(const std::string& context_id,
                                                           int64_t expected_revision) {
    domain::ContextSnapshot ctx;
    ctx.context_id = context_id;
    if (context_id.empty()) {
        ctx.present = false;
        ctx.absence_reason = "NO_CONTEXT_ID_ON_SIGNAL";
        return ctx;
    }
    const std::string schema = impl_->cfg.wiring.altstrategy_schema;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        const auto res = txn.exec_params(
            "SELECT context_revision, event_time_ms, available_at_ms, expires_at_ms,"
            "       global_universe_status, data_quality, venue_mask, missing_venue_mask,"
            "       regime_divergence, feature_manifest_hash, mark_price, spread_bps,"
            "       funding_rate, fees_bps, top_depth_usdt, liquidity_rank,"
            "       expected_slippage_bps, snapshot::text"
            "  FROM " + schema + ".decision_context_snapshots WHERE context_id = $1",
            context_id);
        if (res.empty()) {
            ctx.present = false;
            // Retention on AltStrategy's side is 14 days; a replayed old signal
            // legitimately finds nothing here. That is reported, not filled in.
            ctx.absence_reason = "CONTEXT_SNAPSHOT_NOT_FOUND";
            return;
        }
        const auto& row = res[0];
        ctx.context_revision = row[0].as<int64_t>();
        // Point-in-time correctness is the whole reason this row is read. A
        // revision other than the one the decision was taken on describes a
        // different market, so it is refused rather than transmitted.
        if (expected_revision != 0 && ctx.context_revision != expected_revision) {
            ctx.present = false;
            ctx.absence_reason = "CONTEXT_REVISION_MISMATCH";
            return;
        }
        ctx.present = true;
        ctx.event_time_ms = row[1].as<int64_t>();
        ctx.available_at_ms = row[2].as<int64_t>();
        ctx.expires_at_ms = row[3].as<int64_t>();
        ctx.global_universe_status = row[4].as<std::string>();
        ctx.data_quality = row[5].as<std::string>();
        ctx.venue_mask = static_cast<uint32_t>(row[6].as<int64_t>());
        ctx.missing_venue_mask = static_cast<uint32_t>(row[7].as<int64_t>());
        ctx.regime_divergence = row[8].as<bool>();
        ctx.feature_manifest_hash = row[9].as<std::string>();
        ctx.mark_price = opt_double(row[10]);
        ctx.spread_bps = opt_double(row[11]);
        ctx.funding_rate = opt_double(row[12]);
        ctx.fees_bps = opt_double(row[13]);
        ctx.top_depth_usdt = opt_double(row[14]);
        ctx.liquidity_rank = opt_double(row[15]);
        ctx.expected_slippage_bps = opt_double(row[16]);

        const auto snapshot_text = row[17].as<std::string>();
        nlohmann::json snap = nlohmann::json::parse(snapshot_text, nullptr, false);
        if (snap.is_discarded() || !snap.is_object()) {
            // The scalar columns above are still valid; only the analytics are
            // lost. Reporting a partially present snapshot honestly beats
            // discarding the venue state that did decode.
            ctx.absence_reason = "CONTEXT_ANALYTICS_UNPARSEABLE";
            return;
        }
        if (snap.contains("bitget_features")) {
            collect_named(snap.at("bitget_features"), ctx.venue_features);
        }
        if (snap.contains("cross_venue_features")) {
            collect_named(snap.at("cross_venue_features"), ctx.cross_venue_features);
        }
        if (snap.contains("reference_features")) {
            collect_named(snap.at("reference_features"), ctx.reference_features);
        }
        if (snap.contains("microstructure")) {
            collect_named(snap.at("microstructure"), ctx.microstructure);
        }
        if (snap.contains("bitget_regime_view")) {
            ctx.venue_regime = parse_regime(snap.at("bitget_regime_view"));
        }
        if (snap.contains("global_regime_view")) {
            ctx.global_regime = parse_regime(snap.at("global_regime_view"));
        }
    });
    if (!r) return std::unexpected(r.error());
    return ctx;
}

Result<domain::InstrumentResolution> JournalStore::resolve_instrument(int64_t canonical_pair_id,
                                                                      int64_t as_of_ms) {
    domain::InstrumentResolution inst;
    inst.exchange = impl_->cfg.wiring.execution_exchange;
    inst.resolved_at_ms = timeutil::now_ms();
    const std::string schema = impl_->cfg.wiring.altspotter_schema;

    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        // The executable identity: canonical pair -> venue mapping -> venue
        // instrument. Note what is NOT used: the proposal's own
        // `bitget_instrument_id`. On this deployment that field carries the
        // CANONICAL pair id, not the venue instrument id, so joining on it
        // resolves pair 4 (XRP) to instrument 4 (BTCUSDT). The mapping table is
        // the registry that actually owns this relation.
        const auto res = txn.exec_params(
            "SELECT ei.id, ei.exchange_symbol, ei.status, ei.is_whitelisted,"
            "       cp.base_asset, cp.quote_asset, cp.market_type"
            "  FROM " + schema + ".canonical_pair_exchange_map m"
            "  JOIN " + schema + ".exchange_instruments ei ON ei.id = m.exchange_instrument_id"
            "  JOIN " + schema + ".canonical_pairs cp ON cp.id = m.canonical_pair_id"
            "  JOIN " + schema + ".exchanges ex ON ex.id = m.exchange_id"
            " WHERE m.canonical_pair_id = $1 AND ex.code = $2",
            canonical_pair_id, impl_->cfg.wiring.execution_exchange);
        if (res.empty()) {
            inst.present = false;
            inst.absence_reason = "NO_VENUE_MAPPING_FOR_CANONICAL_PAIR";
            return;
        }
        inst.present = true;
        inst.exchange_instrument_id = res[0][0].as<int64_t>();
        inst.exchange_symbol = res[0][1].as<std::string>();
        inst.instrument_status = res[0][2].as<std::string>();
        inst.whitelisted = res[0][3].as<bool>();
        inst.base_asset = res[0][4].as<std::string>();
        inst.quote_asset = res[0][5].as<std::string>();
        inst.market_type = res[0][6].as<std::string>();

        // The spec version in force AT T0, not the newest one. The plan
        // geometry was computed against the venue's rules as they stood when
        // the decision was taken; handing a consumer a tick size that only came
        // into force afterwards would silently re-round a price the strategy
        // chose.
        const auto spec = txn.exec_params(
            "SELECT version, spec::text, extract(epoch FROM valid_from) * 1000"
            "  FROM " + schema + ".instrument_spec_versions"
            " WHERE exchange_instrument_id = $1 AND valid_from <= to_timestamp($2 / 1000.0)"
            " ORDER BY valid_from DESC, version DESC LIMIT 1",
            inst.exchange_instrument_id, as_of_ms);
        if (spec.empty()) {
            inst.spec_present = false;
            inst.spec_absence_reason = "NO_INSTRUMENT_SPEC_AS_OF_SIGNAL_TIME";
            return;
        }
        inst.spec_version = spec[0][0].as<int64_t>();
        inst.spec_valid_from_ms = static_cast<int64_t>(spec[0][2].as<double>());
        nlohmann::json j = nlohmann::json::parse(spec[0][1].as<std::string>(), nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            inst.spec_present = false;
            inst.spec_absence_reason = "INSTRUMENT_SPEC_UNPARSEABLE";
            return;
        }
        inst.spec_present = true;
        inst.contract_type = j.value("contract_type", "");
        inst.settle_asset = j.value("settle_asset", "");
        inst.tick_size = decimal_string(j, "tick_size");
        inst.quantity_step = decimal_string(j, "quantity_step");
        inst.min_order_quantity = decimal_string(j, "min_order_quantity");
        inst.min_notional = decimal_string(j, "min_notional");
        inst.contract_value = decimal_string(j, "contract_value");
        inst.price_precision = opt_int32(j, "price_precision");
        inst.quantity_precision = opt_int32(j, "quantity_precision");
        inst.max_leverage = opt_double_json(j, "max_leverage");
    });
    if (!r) return std::unexpected(r.error());
    return inst;
}


// ---------------------------------------------------------------------------
// macro-core as of T0 (read-only, AltTech over postgres_fdw)
// ---------------------------------------------------------------------------

Result<domain::MacroSnapshot> JournalStore::load_macro_asof(int64_t t0_ms, int64_t max_age_ms) {
    domain::MacroSnapshot m;
    const std::string schema = impl_->cfg.wiring.alttech_schema;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        // The newest minute at or before T0. The FDW pushes the ORDER BY/LIMIT
        // down, so this is one indexed probe on the producer's side.
        const auto res = txn.exec_params(
            "SELECT v.feature_set_version_id, fs.name, fs.version, fs.schema_hash,"
            "       v.method_version,"
            "       (extract(epoch FROM v.bar_open_time) * 1000)::bigint,"
            "       (extract(epoch FROM v.computed_at) * 1000)::bigint,"
            "       v.status, v.valid_count, v.values::text, encode(v.validity_bitmap,'hex')"
            "  FROM " + schema + ".macro_feature_vectors v"
            "  JOIN " + schema + ".feature_set_versions fs ON fs.id = v.feature_set_version_id"
            " WHERE v.timeframe = '1m' AND fs.scope = 'GLOBAL'"
            "   AND v.bar_open_time <= to_timestamp($1::bigint / 1000.0)"
            "   AND v.computed_at <= to_timestamp($1::bigint / 1000.0)"
            "   AND v.bar_open_time >  to_timestamp(($1::bigint - $2::bigint) / 1000.0)"
            " ORDER BY v.bar_open_time DESC LIMIT 1",
            t0_ms, max_age_ms);
        if (res.empty()) {
            m.present = false;
            m.absence_reason = "NO_MACRO_VECTOR_WITHIN_MAX_AGE";
            return;
        }
        const auto& row = res[0];
        const int64_t set_id = row[0].as<int64_t>();
        m.feature_set_name = row[1].as<std::string>();
        m.feature_set_version = row[2].as<int32_t>();
        m.schema_hash = row[3].as<std::string>();
        m.method_version = row[4].as<std::string>();
        m.bar_open_time_ms = row[5].as<int64_t>();
        m.computed_at_ms = row[6].as<int64_t>();
        m.age_ms = t0_ms - m.bar_open_time_ms;
        m.status = row[7].as<std::string>();
        m.valid_count = row[8].as<int32_t>();
        if (m.status == "WARMING") {
            m.present = false;
            m.absence_reason = "MACRO_VECTOR_WARMING";
            return;
        }

        // Member names, by position, from the producer's registry. Cached: the
        // set is frozen and the lookup is a foreign round trip.
        std::vector<std::string> names;
        {
            std::lock_guard lock(impl_->macro_names_mu);
            if (const auto it = impl_->macro_names.find(set_id); it != impl_->macro_names.end()) {
                names = it->second;
            }
        }
        if (names.empty()) {
            const auto nres = txn.exec_params(
                "SELECT position, output_name FROM " + schema + ".feature_set_members"
                " WHERE feature_set_version_id = $1 ORDER BY position",
                set_id);
            for (const auto& nrow : nres) {
                const auto pos = static_cast<size_t>(nrow[0].as<int64_t>());
                if (names.size() <= pos) names.resize(pos + 1);
                names[pos] = nrow[1].as<std::string>();
            }
            std::lock_guard lock(impl_->macro_names_mu);
            impl_->macro_names[set_id] = names;
        }

        // values::text is "{v1,v2,...}"; the bitmap is bit i -> position i.
        std::vector<double> values;
        {
            const std::string text = row[9].as<std::string>();
            std::string num;
            for (size_t i = 1; i < text.size(); ++i) {
                const char c = text[i];
                if (c == ',' || c == '}') {
                    if (!num.empty()) values.push_back(std::strtod(num.c_str(), nullptr));
                    num.clear();
                } else {
                    num.push_back(c);
                }
            }
        }
        const std::string bitmap = pgbytea::from_hex_literal("\\x" + row[10].as<std::string>());
        const size_t n = std::min(values.size(), names.size());
        if (names.size() != values.size()) {
            // The registry and the vector disagree on width: a position-to-name
            // mapping cannot be trusted, and a mislabelled number is worse than
            // none.
            m.present = false;
            m.absence_reason = "MACRO_WIDTH_MISMATCH";
            return;
        }
        m.feature_count = static_cast<int32_t>(n);
        for (size_t i = 0; i < n; ++i) {
            const bool valid = (i / 8) < bitmap.size() &&
                               ((static_cast<uint8_t>(bitmap[i / 8]) >> (i % 8)) & 1U) != 0U;
            if (valid && std::isfinite(values[i])) {
                m.values.push_back(domain::NamedScalar{names[i], values[i]});
            } else {
                m.unavailable.push_back(names[i]);
            }
        }
        std::sort(m.values.begin(), m.values.end(),
                  [](const auto& a, const auto& b) { return a.name < b.name; });
        std::sort(m.unavailable.begin(), m.unavailable.end());
        m.present = true;
    });
    if (!r) return std::unexpected(r.error());
    return m;
}

// ---------------------------------------------------------------------------
// query path (protocol 2)
// ---------------------------------------------------------------------------

Result<std::optional<JournalRecord>> JournalStore::load_record(const std::string& signal_id) {
    std::optional<JournalRecord> out;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        const auto res = txn.exec_params(
            std::string(kJournalRecordColumns) +
                "  FROM altsignalgateway.signal_journal WHERE signal_id = $1",
            signal_id);
        if (!res.empty()) out = parse_record(res[0]);
    });
    if (!r) return std::unexpected(r.error());
    return out;
}

Result<std::optional<JournalRecord>> JournalStore::load_record_by_sequence(int64_t sequence) {
    std::optional<JournalRecord> out;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        const auto res = txn.exec_params(
            std::string(kJournalRecordColumns) +
                "  FROM altsignalgateway.signal_journal WHERE sequence = $1",
            sequence);
        if (!res.empty()) out = parse_record(res[0]);
    });
    if (!r) return std::unexpected(r.error());
    return out;
}

Result<JournalStore::HistoryPage> JournalStore::history(const HistoryQuery& q) {
    HistoryPage page;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        // Every filter is a bound parameter; the SQL text is fixed. An unset
        // filter is expressed as "$n IS NULL OR column = $n", which the planner
        // resolves per call, so an indexed filter still uses its index.
        const auto res = txn.exec_params(
            std::string(kJournalRecordColumns) +
                "  FROM altsignalgateway.signal_journal"
                " WHERE ($1::text IS NULL OR symbol = $1)"
                "   AND ($2::bigint IS NULL OR canonical_pair_id = $2)"
                "   AND ($3::text IS NULL OR exchange = $3)"
                "   AND ($4::text IS NULL OR strategy_id = $4)"
                "   AND ($5::text IS NULL OR side = $5)"
                "   AND ($6::text IS NULL OR delivery_state = $6)"
                "   AND ($7::bigint IS NULL OR created_at_ms >= $7)"
                "   AND ($8::bigint IS NULL OR created_at_ms <  $8)"
                "   AND ($9::bigint IS NULL OR sequence < $9)"
                " ORDER BY sequence DESC LIMIT $10",
            q.symbol.empty() ? std::optional<std::string>{} : std::optional<std::string>{q.symbol},
            q.canonical_pair_id > 0 ? std::optional<int64_t>{q.canonical_pair_id}
                                    : std::optional<int64_t>{},
            q.exchange.empty() ? std::optional<std::string>{} : std::optional<std::string>{q.exchange},
            q.strategy_id.empty() ? std::optional<std::string>{}
                                  : std::optional<std::string>{q.strategy_id},
            q.side.empty() ? std::optional<std::string>{} : std::optional<std::string>{q.side},
            q.delivery_state.empty() ? std::optional<std::string>{}
                                     : std::optional<std::string>{q.delivery_state},
            q.from_created_at_ms > 0 ? std::optional<int64_t>{q.from_created_at_ms}
                                     : std::optional<int64_t>{},
            q.to_created_at_ms > 0 ? std::optional<int64_t>{q.to_created_at_ms}
                                   : std::optional<int64_t>{},
            q.before_sequence > 0 ? std::optional<int64_t>{q.before_sequence}
                                  : std::optional<int64_t>{},
            q.page_size + 1);
        page.records.reserve(res.size());
        for (const auto& row : res) page.records.push_back(parse_record(row));
        if (page.records.size() > static_cast<size_t>(q.page_size)) {
            page.records.pop_back();
            page.next_before_sequence = page.records.back().sequence;
        }
        const auto head = txn.exec(
            "SELECT COALESCE(last_sequence,0) FROM altsignalgateway.sequence_state WHERE singleton");
        page.head_sequence = head.empty() ? 0 : head[0][0].as<int64_t>();
    });
    if (!r) return std::unexpected(r.error());
    return page;
}

Result<domain::ExtendedContext> JournalStore::load_extended(
    int64_t canonical_pair_id, int64_t t0_ms, bool full,
    const std::vector<CandleWindowSpec>& candles) {
    domain::ExtendedContext ctx;
    ctx.t0_ms = t0_ms;
    ctx.read_at_ms = timeutil::now_ms();
    const std::string schema = impl_->cfg.wiring.altspotter_schema;
    // The last minute that had CLOSED by T0: open_time <= T0 - 60 s. The
    // minute T0 falls inside was still forming when the decision was taken.
    const int64_t last_closed_ms = ((t0_ms - 60'000) / 60'000) * 60'000;

    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        // The market tables key on (exchange_id, instrument_id) where
        // instrument_id is AltSpotter's CANONICAL pair id -- the same number on
        // every venue (its Envelope documents "canonical_pair_id ==
        // instrument_id for usdt_perp pairs"), NOT exchange_instruments.id.
        // The mapping table is consulted only for which venues list the pair.
        struct VenueRow { std::string code; int64_t exchange_id; int64_t instrument_id; };
        std::vector<VenueRow> venues;
        {
            const auto res = txn.exec_params(
                "SELECT ex.code, ex.id"
                "  FROM " + schema + ".canonical_pair_exchange_map m"
                "  JOIN " + schema + ".exchanges ex ON ex.id = m.exchange_id"
                " WHERE m.canonical_pair_id = $1 ORDER BY ex.code",
                canonical_pair_id);
            for (const auto& row : res) {
                venues.push_back({row[0].as<std::string>(), row[1].as<int64_t>(),
                                  canonical_pair_id});
            }
        }
        if (venues.empty()) {
            ctx.absent.push_back("venues:NO_VENUE_MAPPING_FOR_CANONICAL_PAIR");
            return;
        }

        for (const auto& v : venues) {
            // --- derivatives ---------------------------------------------
            {
                const auto res = txn.exec_params(
                    "SELECT (extract(epoch FROM open_time) * 1000)::bigint, mark_price,"
                    "       index_price, funding_rate,"
                    "       (extract(epoch FROM next_funding_time) * 1000)::bigint,"
                    "       open_interest_base, open_interest_quote, best_bid, best_ask,"
                    "       spread, turnover_24h, quality_status"
                    "  FROM " + schema + ".market_derivatives_context_1m"
                    " WHERE exchange_id = $1 AND instrument_id = $2"
                    "   AND open_time <= to_timestamp($3::bigint / 1000.0)"
                    "   AND open_time >  to_timestamp($3::bigint / 1000.0) - interval '30 minutes'"
                    " ORDER BY open_time DESC LIMIT 1",
                    v.exchange_id, v.instrument_id, last_closed_ms);
                if (res.empty()) {
                    ctx.absent.push_back("derivatives:" + v.code + ":NO_ROW_WITHIN_30_MINUTES");
                } else {
                    const auto& row = res[0];
                    domain::DerivativesSnapshot d;
                    d.exchange = v.code;
                    d.open_time_ms = row[0].as<int64_t>();
                    d.age_ms = t0_ms - d.open_time_ms;
                    d.mark_price = opt_double(row[1]);
                    d.index_price = opt_double(row[2]);
                    d.funding_rate = opt_double(row[3]);
                    d.next_funding_time_ms = opt_int64(row[4]);
                    d.open_interest_base = opt_double(row[5]);
                    d.open_interest_quote = opt_double(row[6]);
                    d.best_bid = opt_double(row[7]);
                    d.best_ask = opt_double(row[8]);
                    d.spread = opt_double(row[9]);
                    d.turnover_24h = opt_double(row[10]);
                    d.quality_status = row[11].is_null() ? "" : row[11].as<std::string>();
                    d.source_table = "altspotter.market_derivatives_context_1m";
                    ctx.derivatives.push_back(std::move(d));
                }
            }
            if (!full) continue;
            // --- book ----------------------------------------------------
            {
                const auto res = txn.exec_params(
                    "SELECT (extract(epoch FROM open_time) * 1000)::bigint, best_bid, best_ask,"
                    "       mid_price, spread, relative_spread, bid_depth_5, ask_depth_5,"
                    "       bid_depth_10, ask_depth_10, book_imbalance_5, book_imbalance_10,"
                    "       microprice, snapshot_count, quality_status"
                    "  FROM " + schema + ".market_book_aggregates_1m"
                    " WHERE exchange_id = $1 AND instrument_id = $2"
                    "   AND open_time <= to_timestamp($3::bigint / 1000.0)"
                    "   AND open_time >  to_timestamp($3::bigint / 1000.0) - interval '30 minutes'"
                    " ORDER BY open_time DESC LIMIT 1",
                    v.exchange_id, v.instrument_id, last_closed_ms);
                if (res.empty()) {
                    ctx.absent.push_back("book:" + v.code + ":NO_ROW_WITHIN_30_MINUTES");
                } else {
                    const auto& row = res[0];
                    domain::BookSnapshot b;
                    b.exchange = v.code;
                    b.open_time_ms = row[0].as<int64_t>();
                    b.age_ms = t0_ms - b.open_time_ms;
                    b.best_bid = opt_double(row[1]);
                    b.best_ask = opt_double(row[2]);
                    b.mid_price = opt_double(row[3]);
                    b.spread = opt_double(row[4]);
                    b.relative_spread = opt_double(row[5]);
                    b.bid_depth_5 = opt_double(row[6]);
                    b.ask_depth_5 = opt_double(row[7]);
                    b.bid_depth_10 = opt_double(row[8]);
                    b.ask_depth_10 = opt_double(row[9]);
                    b.book_imbalance_5 = opt_double(row[10]);
                    b.book_imbalance_10 = opt_double(row[11]);
                    b.microprice = opt_double(row[12]);
                    b.snapshot_count = row[13].is_null() ? 0 : row[13].as<int64_t>();
                    b.quality_status = row[14].is_null() ? "" : row[14].as<std::string>();
                    b.source_table = "altspotter.market_book_aggregates_1m";
                    ctx.book.push_back(std::move(b));
                }
            }
            // --- trade flow -----------------------------------------------
            {
                const auto res = txn.exec_params(
                    "SELECT (extract(epoch FROM open_time) * 1000)::bigint, trade_count,"
                    "       buy_trade_count, sell_trade_count, base_volume, quote_volume,"
                    "       aggressive_buy_volume, aggressive_sell_volume, signed_volume, vwap,"
                    "       largest_trade, quality_status"
                    "  FROM " + schema + ".market_trade_aggregates_1m"
                    " WHERE exchange_id = $1 AND instrument_id = $2"
                    "   AND open_time <= to_timestamp($3::bigint / 1000.0)"
                    "   AND open_time >  to_timestamp($3::bigint / 1000.0) - interval '30 minutes'"
                    " ORDER BY open_time DESC LIMIT 1",
                    v.exchange_id, v.instrument_id, last_closed_ms);
                if (res.empty()) {
                    ctx.absent.push_back("flow:" + v.code + ":NO_ROW_WITHIN_30_MINUTES");
                } else {
                    const auto& row = res[0];
                    domain::FlowSnapshot f;
                    f.exchange = v.code;
                    f.open_time_ms = row[0].as<int64_t>();
                    f.age_ms = t0_ms - f.open_time_ms;
                    f.trade_count = row[1].is_null() ? 0 : row[1].as<int64_t>();
                    f.buy_trade_count = row[2].is_null() ? 0 : row[2].as<int64_t>();
                    f.sell_trade_count = row[3].is_null() ? 0 : row[3].as<int64_t>();
                    f.base_volume = opt_double(row[4]);
                    f.quote_volume = opt_double(row[5]);
                    f.aggressive_buy_volume = opt_double(row[6]);
                    f.aggressive_sell_volume = opt_double(row[7]);
                    f.signed_volume = opt_double(row[8]);
                    f.vwap = opt_double(row[9]);
                    f.largest_trade = opt_double(row[10]);
                    f.quality_status = row[11].is_null() ? "" : row[11].as<std::string>();
                    f.source_table = "altspotter.market_trade_aggregates_1m";
                    ctx.flow.push_back(std::move(f));
                }
            }
        }
        if (!full) return;

        // --- candles: the execution venue only, fixed windows ending at the
        //     last bar closed by T0 ----------------------------------------
        const VenueRow* exec = nullptr;
        for (const auto& v : venues) {
            if (v.code == impl_->cfg.wiring.execution_exchange) exec = &v;
        }
        if (exec == nullptr) {
            ctx.absent.push_back("candles:NO_EXECUTION_VENUE_MAPPING");
            return;
        }
        for (const auto& spec : candles) {
            if (spec.bars <= 0) continue;
            domain::CandleWindow w;
            w.exchange = exec->code;
            w.timeframe = spec.timeframe;
            w.requested_bars = spec.bars;
            pqxx::result res;
            if (spec.timeframe == "1m") {
                w.source_table = "altspotter.market_candles_1m";
                res = txn.exec_params(
                    "SELECT (extract(epoch FROM open_time) * 1000)::bigint, open, high, low,"
                    "       close, base_volume, quote_volume, trade_count,"
                    "       taker_buy_base_volume, is_closed, quality_status"
                    "  FROM " + schema + ".market_candles_1m"
                    " WHERE exchange_id = $1 AND instrument_id = $2"
                    "   AND open_time <= to_timestamp($3::bigint / 1000.0)"
                    "   AND open_time >  to_timestamp($3::bigint / 1000.0) - ($4::int * interval '1 minute')"
                    " ORDER BY open_time DESC LIMIT $4",
                    exec->exchange_id, exec->instrument_id, last_closed_ms, spec.bars * 2);
            } else {
                w.source_table = "altspotter.market_candles_agg";
                const int64_t span_min = spec.timeframe == "5m" ? 5 : 60;
                // The last bar of this timeframe that had closed by T0.
                const int64_t last_tf = ((t0_ms - span_min * 60'000) / (span_min * 60'000)) *
                                        (span_min * 60'000);
                res = txn.exec_params(
                    "SELECT (extract(epoch FROM open_time) * 1000)::bigint, open, high, low,"
                    "       close, base_volume, quote_volume, trade_count,"
                    "       taker_buy_base_volume, is_closed, quality_status"
                    "  FROM " + schema + ".market_candles_agg"
                    " WHERE exchange_id = $1 AND instrument_id = $2 AND timeframe = $5"
                    "   AND open_time <= to_timestamp($3::bigint / 1000.0)"
                    "   AND open_time >  to_timestamp($3::bigint / 1000.0) - ($4::int * interval '1 minute')"
                    " ORDER BY open_time DESC LIMIT $6",
                    exec->exchange_id, exec->instrument_id, last_tf,
                    static_cast<int64_t>(spec.bars) * span_min * 2, spec.timeframe, spec.bars);
            }
            for (auto it = res.rbegin(); it != res.rend(); ++it) {
                const auto& row = *it;
                domain::CandleBar bar;
                bar.open_time_ms = row[0].as<int64_t>();
                bar.open = row[1].as<double>();
                bar.high = row[2].as<double>();
                bar.low = row[3].as<double>();
                bar.close = row[4].as<double>();
                bar.base_volume = row[5].is_null() ? 0.0 : row[5].as<double>();
                bar.quote_volume = row[6].is_null() ? 0.0 : row[6].as<double>();
                bar.trade_count = row[7].is_null() ? 0 : row[7].as<int64_t>();
                bar.taker_buy_base_volume = opt_double(row[8]);
                bar.is_closed = !row[9].is_null() && row[9].as<bool>();
                bar.quality_status = row[10].is_null() ? "" : row[10].as<std::string>();
                w.bars.push_back(std::move(bar));
            }
            if (w.bars.size() > static_cast<size_t>(spec.bars)) {
                w.bars.erase(w.bars.begin(), w.bars.end() - spec.bars);
            }
            if (w.bars.empty()) ctx.absent.push_back("candles:" + spec.timeframe + ":NO_ROWS");
            ctx.candles.push_back(std::move(w));
        }
    });
    if (!r) return std::unexpected(r.error());
    return ctx;
}

Result<void> JournalStore::record_rejected(const std::string& signal_id,
                                           const std::string& subject, uint64_t stream_sequence,
                                           const std::string& code, const std::string& detail,
                                           const std::string& payload, int64_t now_ms) {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altsignalgateway.rejected_messages"
            " (signal_id, subject, stream_sequence, rejection_code, detail, payload,"
            "  payload_sha256, payload_bytes, received_at_ms)"
            " VALUES ($1,$2,$3,$4,$5,$6::bytea,$7,$8,$9)",
            signal_id, subject, static_cast<int64_t>(stream_sequence), code, detail,
            pgbytea::hex_literal(payload), sha256_hex(payload),
            static_cast<int32_t>(payload.size()), now_ms);
        txn.exec_params(
            "UPDATE altsignalgateway.service_state"
            "   SET signals_rejected = signals_rejected + 1, updated_at = now() WHERE singleton");
    });
}

Result<void> JournalStore::record_integrity_violation(
    const std::string& signal_id, const std::string& existing_sha256,
    const std::string& incoming_sha256, int64_t existing_sequence, const std::string& subject,
    uint64_t stream_sequence, const std::string& payload, int64_t now_ms) {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altsignalgateway.integrity_violations"
            " (signal_id, existing_sha256, incoming_sha256, existing_sequence, subject,"
            "  stream_sequence, incoming_payload, detected_at_ms)"
            " VALUES ($1,$2,$3,$4,$5,$6,$7::bytea,$8)",
            signal_id, existing_sha256, incoming_sha256, existing_sequence, subject,
            static_cast<int64_t>(stream_sequence), pgbytea::hex_literal(payload), now_ms);
    });
}

Result<void> JournalStore::open_session(const std::string& session_id,
                                        const std::string& client_id, const std::string& key_id,
                                        const std::string& peer, int32_t protocol_version,
                                        const std::string& client_version, int64_t now_ms) {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altsignalgateway.sessions"
            " (session_id, client_id, key_id, peer_address, protocol_version, client_version,"
            "  opened_at_ms) VALUES ($1,$2,$3,$4,$5,$6,$7)"
            " ON CONFLICT (session_id) DO NOTHING",
            session_id, client_id, key_id, peer, protocol_version, client_version, now_ms);
    });
}

Result<void> JournalStore::note_session_resume(const std::string& session_id,
                                               const std::string& mode, int64_t resume_from,
                                               int64_t replayed) {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "UPDATE altsignalgateway.sessions"
            "   SET resume_mode = $2, resume_from = $3, replayed_count = $4"
            " WHERE session_id = $1",
            session_id, mode, resume_from, replayed);
    });
}

Result<void> JournalStore::close_session(const std::string& session_id, const std::string& reason,
                                         int64_t delivered, int64_t acked, int64_t now_ms) {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "UPDATE altsignalgateway.sessions"
            "   SET closed_at_ms = $2, close_reason = $3, delivered_count = $4, acked_count = $5"
            " WHERE session_id = $1",
            session_id, now_ms, reason, delivered, acked);
    });
}

Result<JournalStats> JournalStore::stats() {
    JournalStats s;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        const auto res = txn.exec(
            "SELECT COALESCE(max(sequence),0), count(*),"
            "       count(*) FILTER (WHERE delivery_state='JOURNALED'),"
            "       count(*) FILTER (WHERE delivery_state='QUEUED'),"
            "       count(*) FILTER (WHERE delivery_state='SENT'),"
            "       count(*) FILTER (WHERE delivery_state='ACKED'),"
            "       count(*) FILTER (WHERE delivery_state='EXPIRED'),"
            "       count(*) FILTER (WHERE delivery_state='FAILED'),"
            "       COALESCE(max(journaled_at_ms),0), COALESCE(max(last_ack_at_ms),0)"
            "  FROM altsignalgateway.signal_journal");
        s.head_sequence = res[0][0].as<int64_t>();
        s.total_rows = res[0][1].as<int64_t>();
        s.journaled = res[0][2].as<int64_t>();
        s.queued = res[0][3].as<int64_t>();
        s.sent = res[0][4].as<int64_t>();
        s.acked = res[0][5].as<int64_t>();
        s.expired = res[0][6].as<int64_t>();
        s.failed = res[0][7].as<int64_t>();
        s.last_signal_at_ms = res[0][8].as<int64_t>();
        s.last_ack_at_ms = res[0][9].as<int64_t>();
    });
    if (!r) return std::unexpected(r.error());
    return s;
}

Result<int64_t> JournalStore::head_sequence() {
    int64_t head = 0;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        head = txn.query_value<int64_t>(
            "SELECT COALESCE(last_sequence,0) FROM altsignalgateway.sequence_state WHERE singleton");
    });
    if (!r) return std::unexpected(r.error());
    return head;
}

Result<JournalStore::Backlog> JournalStore::backlog_above(int64_t sequence, int64_t now_ms) {
    Backlog b;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        const auto res = txn.exec_params(
            "SELECT count(*) FILTER (WHERE expires_at_ms > $2"
            "                        AND delivery_state NOT IN ('EXPIRED','FAILED')),"
            "       count(*) FILTER (WHERE expires_at_ms <= $2"
            "                        OR delivery_state = 'EXPIRED')"
            "  FROM altsignalgateway.signal_journal WHERE sequence > $1",
            sequence, now_ms);
        b.deliverable = res[0][0].as<int64_t>();
        b.expired = res[0][1].as<int64_t>();
    });
    if (!r) return std::unexpected(r.error());
    return b;
}

Result<void> JournalStore::reconcile_retention_policies() {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        const auto set = [&](const char* table, int32_t days) {
            const auto res = txn.exec_params(
                "UPDATE altsignalgateway.retention_policies"
                "   SET retain_days = $2, updated_at = now()"
                " WHERE table_name = $1 AND retain_days <> $2 RETURNING retain_days",
                table, days);
            if (!res.empty()) {
                log::info("retention_policy_updated", {{"table", table}, {"retain_days", days}});
            }
        };
        set("signal_journal", impl_->cfg.journal.retain_days);
        set("rejected_messages", impl_->cfg.journal.rejected_retain_days);
    });
}

Result<int64_t> JournalStore::retention_pass() {
    int64_t deleted = 0;
    auto r = impl_->pool.with_txn([&](pqxx::work& txn) {
        const auto run_id = txn.query_value<int64_t>(
            "INSERT INTO altsignalgateway.retention_runs DEFAULT VALUES RETURNING id");
        // The journal DELETE trigger refuses anything that is not this pass.
        txn.exec("SET LOCAL altsignalgateway.retention_pass = 'on'");
        nlohmann::json detail = nlohmann::json::object();
        const auto policies = txn.exec(
            "SELECT table_name, retain_days, time_column FROM altsignalgateway.retention_policies");
        for (const auto& p : policies) {
            const auto table = p[0].as<std::string>();
            const auto days = p[1].as<int32_t>();
            const auto column = p[2].as<std::string>();
            // table_name and time_column come from a table this service owns and
            // seeds; they are still validated so a future row cannot smuggle SQL.
            const auto safe = [](const std::string& s) {
                return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
                    return std::isalnum(c) != 0 || c == '_';
                });
            };
            if (!safe(table) || !safe(column)) continue;
            const auto res = txn.exec(
                "DELETE FROM altsignalgateway." + table + " WHERE " + column +
                " < now() - interval '" + std::to_string(days) + " days'");
            const auto n = static_cast<int64_t>(res.affected_rows());
            detail[table] = n;
            deleted += n;
        }
        txn.exec_params(
            "UPDATE altsignalgateway.retention_runs"
            "   SET finished_at = now(), rows_deleted = $2, detail = $3::jsonb WHERE id = $1",
            run_id, deleted, detail.dump());
    });
    if (!r) return std::unexpected(r.error());
    return deleted;
}

Result<void> JournalStore::note_started(int64_t now_ms) {
    return impl_->pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "UPDATE altsignalgateway.service_state"
            "   SET instance_id = $1, started_at_ms = $2, updated_at = now() WHERE singleton",
            impl_->instance_id, now_ms);
    });
}

Result<void> JournalStore::ping() {
    return impl_->pool.with_txn([](pqxx::work& txn) { txn.exec("SELECT 1"); });
}

} // namespace altsignalgateway::data
