#include "altsignalgateway/cli/commands.hpp"

#include "altsignalgateway/core/codec.hpp"
#include "altsignalgateway/core/config.hpp"
#include "altsignalgateway/core/hash.hpp"
#include "altsignalgateway/core/logger.hpp"
#include "altsignalgateway/core/nats_provision.hpp"
#include "altsignalgateway/core/pg.hpp"
#include "altsignalgateway/core/time.hpp"
#include "altsignalgateway/data/pg_bytea.hpp"
#include "altsignalgateway/data/store.hpp"
#include "altsignalgateway/domain/auth.hpp"
#include "altsignalgateway/domain/protocol.hpp"
#include "gateway.pb.h"

#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <cstdio>
#include <filesystem>

namespace altsignalgateway::cli {

namespace pb = altsignalgateway::v1;

namespace {

// Diagnostics only. The wire format is protobuf; JSON exists here so an
// operator can read one signal without a decoder, and nowhere else.
std::string to_json(const google::protobuf::Message& msg, bool pretty) {
    std::string out;
    google::protobuf::util::JsonPrintOptions opts;
    opts.add_whitespace = pretty;
    opts.always_print_primitive_fields = true;
    opts.preserve_proto_field_names = true;
    if (!google::protobuf::util::MessageToJsonString(msg, &out, opts).ok()) {
        return "{\"error\":\"protobuf message could not be rendered as JSON\"}";
    }
    return out;
}

config::RuntimeConfig* load_or_report(const Args& args, config::RuntimeConfig& into) {
    auto cfg = config::load_runtime(args.config);
    if (!cfg) {
        std::fprintf(stderr, "configuration error: %s\n", cfg.error().message.c_str());
        return nullptr;
    }
    into = std::move(*cfg);
    return &into;
}

} // namespace

int cmd_nats_setup(const Args& args) {
    config::RuntimeConfig cfg;
    if (load_or_report(args, cfg) == nullptr) return 1;
    log::init("altsignalgateway", cfg.common.instance_id, cfg.common.log_level);
    log::set_thread_component("nats-setup");
    const bool ok = messaging::nats_provision(cfg.common.nats_url, cfg.wiring,
                                              cfg.wiring.final_deliver_policy == "all");
    return ok ? 0 : 1;
}

int cmd_validate(const Args& args) {
    config::RuntimeConfig cfg;
    if (load_or_report(args, cfg) == nullptr) return 1;

    int failures = 0;
    const auto check = [&failures](const char* what, bool ok, const std::string& detail) {
        std::printf("%-34s %s%s%s\n", what, ok ? "OK" : "FAIL", detail.empty() ? "" : "  ",
                    detail.c_str());
        if (!ok) ++failures;
    };

    check("configuration", true, args.config);
    std::printf("%-34s %s\n", "environment", cfg.common.environment.c_str());
    std::printf("%-34s %s\n", "wiring", cfg.wiring.id.c_str());
    std::printf("%-34s %s / %s\n", "input", cfg.wiring.final_stream.c_str(),
                cfg.wiring.final_durable.c_str());
    std::printf("%-34s %s\n", "execution exchange", cfg.wiring.execution_exchange.c_str());
    std::printf("%-34s %s:%u\n", "websocket listen",
                cfg.websocket.listen_address.c_str(),
                static_cast<unsigned>(cfg.websocket.listen_port));
    std::printf("%-34s %u / schema %u\n", "protocol version", domain::kProtocolVersion,
                domain::kSchemaVersion);

    // --- credentials ---------------------------------------------------------
    auto creds = config::load_credentials(cfg.auth.credentials_file);
    if (!creds) {
        check("credentials", false, creds.error().message);
    } else {
        std::vector<domain::RawCredential> raw;
        for (const auto& c : *creds) {
            raw.push_back(domain::RawCredential{c.client_id, c.key_id, c.secret_hex,
                                                c.capabilities, c.max_sessions});
        }
        auto registry = domain::ClientRegistry::build(raw);
        if (!registry) {
            check("credentials", false, registry.error().message);
        } else {
            check("credentials", true, std::to_string(registry->size()) + " client(s)");
            for (const auto& c : registry->all()) {
                std::string caps;
                for (const auto cap : c.capabilities) {
                    if (!caps.empty()) caps += ",";
                    caps += domain::capability_name(cap);
                }
                // The key id, never the key.
                std::printf("    client %-20s key=%-10s caps=%s max_sessions=%d\n",
                            c.client_id.c_str(), c.key_id.c_str(), caps.c_str(), c.max_sessions);
            }
        }
    }

    // --- TLS material ---------------------------------------------------------
    std::error_code ec;
    const bool cert_ok = std::filesystem::exists(cfg.tls.certificate_chain_file, ec);
    check("tls certificate", cert_ok, cfg.tls.certificate_chain_file);
    const bool key_ok = std::filesystem::exists(cfg.tls.private_key_file, ec);
    check("tls private key", key_ok, cfg.tls.private_key_file);
    if (key_ok) {
        struct stat st{};
        ::stat(cfg.tls.private_key_file.c_str(), &st);
        const bool perm_ok = (st.st_mode & (S_IROTH | S_IWOTH | S_IXOTH)) == 0;
        char mode[8];
        std::snprintf(mode, sizeof(mode), "%04o", st.st_mode & 07777);
        check("tls private key permissions", perm_ok, std::string("mode ") + mode);
    }
    if (cfg.tls.require_client_certificate) {
        check("mutual tls ca bundle", std::filesystem::exists(cfg.tls.client_ca_file, ec),
              cfg.tls.client_ca_file);
    }

    // --- database -------------------------------------------------------------
    persistence::PgPool pool(cfg.common.postgres_dsn, 1);
    auto probe = pool.with_txn([&](pqxx::work& txn) {
        const auto journal = txn.query_value<bool>(
            "SELECT to_regclass('altsignalgateway.signal_journal') IS NOT NULL");
        if (!journal) throw std::runtime_error("altsignalgateway.signal_journal is missing");
        // The two read-only dependencies. A missing grant here means every
        // signal would ship without its market snapshot or without an
        // executable symbol, which is a silent degradation rather than a
        // failure — so it is checked up front.
        txn.exec("SELECT 1 FROM " + cfg.wiring.altstrategy_schema +
                 ".decision_context_snapshots LIMIT 1");
        txn.exec("SELECT 1 FROM " + cfg.wiring.altspotter_schema +
                 ".canonical_pair_exchange_map LIMIT 1");
        txn.exec("SELECT 1 FROM " + cfg.wiring.altspotter_schema +
                 ".instrument_spec_versions LIMIT 1");
    });
    check("database", probe.has_value(), probe ? cfg.wiring.altstrategy_schema + " + " +
                                                     cfg.wiring.altspotter_schema + " readable"
                                               : probe.error().message);

    std::printf("\n%s\n", failures == 0 ? "validate: OK" : "validate: FAILED");
    return failures == 0 ? 0 : 1;
}

int cmd_status(const Args& args) {
    config::RuntimeConfig cfg;
    if (load_or_report(args, cfg) == nullptr) return 1;

    persistence::PgPool pool(cfg.common.postgres_dsn, 1);
    nlohmann::json out;
    out["service"] = "altsignalgateway";
    out["version"] = ALTSIGNALGATEWAY_VERSION;
    out["instance"] = cfg.common.instance_id;
    out["protocol_version"] = domain::kProtocolVersion;
    out["schema_version"] = domain::kSchemaVersion;
    out["generated_at"] = timeutil::to_iso8601(timeutil::now_ms());

    auto r = pool.with_txn([&](pqxx::work& txn) {
        const auto j = txn.exec(
            "SELECT COALESCE(max(sequence),0), count(*),"
            "       count(*) FILTER (WHERE delivery_state='JOURNALED'),"
            "       count(*) FILTER (WHERE delivery_state='QUEUED'),"
            "       count(*) FILTER (WHERE delivery_state='SENT'),"
            "       count(*) FILTER (WHERE delivery_state='ACKED'),"
            "       count(*) FILTER (WHERE delivery_state='EXPIRED'),"
            "       count(*) FILTER (WHERE delivery_state='FAILED'),"
            "       COALESCE(max(journaled_at_ms),0), COALESCE(max(last_ack_at_ms),0),"
            "       COALESCE(pg_total_relation_size('altsignalgateway.signal_journal'),0)"
            "  FROM altsignalgateway.signal_journal");
        nlohmann::json journal;
        journal["head_sequence"] = j[0][0].as<int64_t>();
        journal["rows"] = j[0][1].as<int64_t>();
        journal["journaled"] = j[0][2].as<int64_t>();
        journal["queued"] = j[0][3].as<int64_t>();
        journal["sent"] = j[0][4].as<int64_t>();
        journal["acked"] = j[0][5].as<int64_t>();
        journal["expired"] = j[0][6].as<int64_t>();
        journal["failed"] = j[0][7].as<int64_t>();
        journal["backlog"] = j[0][2].as<int64_t>() + j[0][3].as<int64_t>() + j[0][4].as<int64_t>();
        const auto last_signal = j[0][8].as<int64_t>();
        const auto last_ack = j[0][9].as<int64_t>();
        journal["last_signal_at"] = last_signal > 0 ? timeutil::to_iso8601(last_signal) : "";
        journal["last_ack_at"] = last_ack > 0 ? timeutil::to_iso8601(last_ack) : "";
        journal["bytes_on_disk"] = j[0][10].as<int64_t>();
        out["journal"] = journal;

        const auto s = txn.exec(
            "SELECT instance_id, started_at_ms, signals_journalled, signals_rejected,"
            "       last_signal_id"
            "  FROM altsignalgateway.service_state WHERE singleton");
        if (!s.empty()) {
            nlohmann::json svc;
            svc["instance_id"] = s[0][0].as<std::string>();
            const auto started = s[0][1].as<int64_t>();
            svc["started_at"] = started > 0 ? timeutil::to_iso8601(started) : "";
            svc["signals_journalled"] = s[0][2].as<int64_t>();
            svc["signals_rejected"] = s[0][3].as<int64_t>();
            svc["last_signal_id"] = s[0][4].as<std::string>();
            out["service_state"] = svc;
        }

        nlohmann::json clients = nlohmann::json::array();
        for (const auto& row : txn.exec(
                 "SELECT client_id, last_acked_sequence, protocol_version, client_version,"
                 "       last_seen_at_ms, last_ack_at_ms, total_acked"
                 "  FROM altsignalgateway.client_cursors ORDER BY client_id")) {
            nlohmann::json c;
            c["client_id"] = row[0].as<std::string>();
            c["last_acked_sequence"] = row[1].as<int64_t>();
            c["protocol_version"] = row[2].as<int32_t>();
            c["client_version"] = row[3].as<std::string>();
            const auto seen = row[4].as<int64_t>();
            const auto acked = row[5].as<int64_t>();
            c["last_seen_at"] = seen > 0 ? timeutil::to_iso8601(seen) : "";
            c["last_ack_at"] = acked > 0 ? timeutil::to_iso8601(acked) : "";
            c["total_acked"] = row[6].as<int64_t>();
            clients.push_back(std::move(c));
        }
        out["clients"] = clients;

        nlohmann::json sessions = nlohmann::json::array();
        for (const auto& row : txn.exec(
                 "SELECT session_id, client_id, peer_address, opened_at_ms, resume_mode,"
                 "       resume_from, delivered_count, acked_count"
                 "  FROM altsignalgateway.sessions WHERE closed_at_ms IS NULL"
                 "  ORDER BY opened_at_ms DESC LIMIT 32")) {
            nlohmann::json s2;
            s2["session_id"] = row[0].as<std::string>();
            s2["client_id"] = row[1].as<std::string>();
            s2["peer"] = row[2].as<std::string>();
            s2["opened_at"] = timeutil::to_iso8601(row[3].as<int64_t>());
            s2["resume_mode"] = row[4].as<std::string>();
            s2["resume_from"] = row[5].as<int64_t>();
            s2["delivered"] = row[6].as<int64_t>();
            s2["acked"] = row[7].as<int64_t>();
            sessions.push_back(std::move(s2));
        }
        out["open_sessions"] = sessions;

        out["rejected_messages"] =
            txn.query_value<int64_t>("SELECT count(*) FROM altsignalgateway.rejected_messages");
        out["integrity_violations"] =
            txn.query_value<int64_t>("SELECT count(*) FROM altsignalgateway.integrity_violations");
    });
    if (!r) {
        std::fprintf(stderr, "database error: %s\n", r.error().message.c_str());
        return 1;
    }
    std::printf("%s\n", out.dump(2).c_str());
    return 0;
}

int cmd_inspect(const Args& args) {
    config::RuntimeConfig cfg;
    if (load_or_report(args, cfg) == nullptr) return 1;

    const std::string signal_id = args.option("--signal-id");
    bool ok = true;
    const int64_t sequence_value = args.int_option("--sequence", 0, &ok);
    if (!ok) return 2;
    const std::string sequence = args.option("--sequence");
    const bool want_source = args.flag("--source");
    if (signal_id.empty() && sequence.empty()) {
        std::fprintf(stderr, "inspect requires --signal-id ID or --sequence N\n");
        return 2;
    }

    persistence::PgPool pool(cfg.common.postgres_dsn, 1);
    int rc = 0;
    auto r = pool.with_txn([&](pqxx::work& txn) {
        const auto rows =
            signal_id.empty()
                ? txn.exec_params(
                      "SELECT signal_id, message_id, sequence, delivery_state, delivery_attempts,"
                      "       payload_sha256, source_payload_sha256, encode(canonical_payload,'hex'),"
                      "       encode(source_payload,'hex'), warnings::text, created_at_ms,"
                      "       expires_at_ms, journaled_at_ms"
                      "  FROM altsignalgateway.signal_journal WHERE sequence = $1",
                      sequence_value)
                : txn.exec_params(
                      "SELECT signal_id, message_id, sequence, delivery_state, delivery_attempts,"
                      "       payload_sha256, source_payload_sha256, encode(canonical_payload,'hex'),"
                      "       encode(source_payload,'hex'), warnings::text, created_at_ms,"
                      "       expires_at_ms, journaled_at_ms"
                      "  FROM altsignalgateway.signal_journal WHERE signal_id = $1",
                      signal_id);
        if (rows.empty()) {
            std::fprintf(stderr, "no such journalled signal\n");
            rc = 1;
            return;
        }
        const auto& row = rows[0];
        const auto canonical = pgbytea::from_hex_literal("\\x" + row[7].as<std::string>());

        nlohmann::json head;
        head["signal_id"] = row[0].as<std::string>();
        head["message_id"] = row[1].as<std::string>();
        head["sequence"] = row[2].as<int64_t>();
        head["delivery_state"] = row[3].as<std::string>();
        head["delivery_attempts"] = row[4].as<int32_t>();
        head["payload_sha256"] = row[5].as<std::string>();
        head["source_payload_sha256"] = row[6].as<std::string>();
        head["created_at"] = timeutil::to_iso8601(row[10].as<int64_t>());
        head["expires_at"] = timeutil::to_iso8601(row[11].as<int64_t>());
        head["journaled_at"] = timeutil::to_iso8601(row[12].as<int64_t>());
        head["warnings"] = row[9].as<std::string>();
        // Recomputed here, not trusted: `inspect` is what an operator reaches
        // for when they already suspect something, so it re-verifies rather
        // than echoing the stored value.
        head["payload_sha256_recomputed"] = sha256_hex(canonical);
        head["payload_bytes"] = canonical.size();
        std::printf("%s\n", head.dump(2).c_str());

        if (want_source) {
            const auto source = pgbytea::from_hex_literal("\\x" + row[8].as<std::string>());
            altsignalquality::v1::Envelope env;
            if (!env.ParseFromString(source)) {
                std::fprintf(stderr, "stored source payload does not parse\n");
                rc = 1;
                return;
            }
            std::printf("%s\n", to_json(env, true).c_str());
            return;
        }
        pb::SignalPayload payload;
        if (!payload.ParseFromString(canonical)) {
            std::fprintf(stderr, "stored canonical payload does not parse\n");
            rc = 1;
            return;
        }
        std::printf("%s\n", to_json(payload, true).c_str());
    });
    if (!r) {
        std::fprintf(stderr, "database error: %s\n", r.error().message.c_str());
        return 1;
    }
    return rc;
}

int cmd_verify(const Args& args) {
    config::RuntimeConfig cfg;
    if (load_or_report(args, cfg) == nullptr) return 1;
    bool ok = true;
    // 1000 rows is what a routine spot check costs: a couple of seconds on this
    // stack's volumes. `--limit 0` checks nothing and is how the load harness
    // measures its own fixed overhead.
    const int64_t limit = args.int_option("--limit", 1000, &ok);
    if (!ok) return 2;
    if (limit < 0) {
        std::fprintf(stderr, "--limit must be >= 0\n");
        return 2;
    }

    persistence::PgPool pool(cfg.common.postgres_dsn, 1);
    int64_t checked = 0;
    int64_t bad_payload = 0;
    int64_t bad_source = 0;
    int64_t unparseable = 0;
    int64_t embedded_mismatch = 0;

    auto r = pool.with_txn([&](pqxx::work& txn) {
        for (const auto& row : txn.exec_params(
                 "SELECT signal_id, sequence, payload_sha256, source_payload_sha256,"
                 "       encode(canonical_payload,'hex'), encode(source_payload,'hex')"
                 "  FROM altsignalgateway.signal_journal ORDER BY sequence DESC LIMIT $1",
                 limit)) {
            ++checked;
            const auto signal_id = row[0].as<std::string>();
            const auto canonical = pgbytea::from_hex_literal("\\x" + row[4].as<std::string>());
            const auto source = pgbytea::from_hex_literal("\\x" + row[5].as<std::string>());

            if (sha256_hex(canonical) != row[2].as<std::string>()) {
                ++bad_payload;
                std::printf("PAYLOAD_HASH_MISMATCH  seq=%s signal_id=%s\n",
                            row[1].c_str(), signal_id.c_str());
            }
            if (sha256_hex(source) != row[3].as<std::string>()) {
                ++bad_source;
                std::printf("SOURCE_HASH_MISMATCH   seq=%s signal_id=%s\n",
                            row[1].c_str(), signal_id.c_str());
            }
            pb::SignalPayload payload;
            if (!payload.ParseFromString(canonical)) {
                ++unparseable;
                std::printf("PAYLOAD_UNPARSEABLE    seq=%s signal_id=%s\n",
                            row[1].c_str(), signal_id.c_str());
                continue;
            }
            // The embedded original must be the same bytes as the column, and
            // the provenance hash must describe them. This is what proves the
            // canonical projection and the producer's own message are two views
            // of ONE signal rather than two independent records.
            if (payload.source_envelope() != source ||
                payload.provenance().source_envelope_sha256() != sha256_hex(source)) {
                ++embedded_mismatch;
                std::printf("EMBEDDED_SOURCE_MISMATCH seq=%s signal_id=%s\n",
                            row[1].c_str(), signal_id.c_str());
            }
            // And the canonical serialization must be stable: re-serializing
            // what we just parsed has to reproduce the stored bytes exactly, or
            // the delivery path would send something the hash does not cover.
            auto again = messaging::canonical_serialize(payload);
            if (!again || *again != canonical) {
                ++bad_payload;
                std::printf("CANONICAL_UNSTABLE     seq=%s signal_id=%s\n",
                            row[1].c_str(), signal_id.c_str());
            }
        }
    });
    if (!r) {
        std::fprintf(stderr, "database error: %s\n", r.error().message.c_str());
        return 1;
    }
    const int64_t failures = bad_payload + bad_source + unparseable + embedded_mismatch;
    std::printf("checked=%lld payload_hash_bad=%lld source_hash_bad=%lld unparseable=%lld "
                "embedded_mismatch=%lld -> %s\n",
                static_cast<long long>(checked), static_cast<long long>(bad_payload),
                static_cast<long long>(bad_source), static_cast<long long>(unparseable),
                static_cast<long long>(embedded_mismatch), failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}

} // namespace altsignalgateway::cli
