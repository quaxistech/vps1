#pragma once

#include "altsignalgateway/core/error.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <string>
#include <vector>

namespace altsignalgateway::config {

// Load YAML file and expand ${ENV_VAR} placeholders in all scalar values.
// A missing environment variable is an error: secrets must be explicit, never
// silently defaulted to an empty string.
Result<YAML::Node> load_yaml(const std::string& path);

struct CommonConfig {
    std::string environment;
    std::string log_level = "info";
    std::string timezone = "UTC";
    std::string instance_id;
    std::string nats_url;
    std::string postgres_dsn;
    int32_t shutdown_timeout_sec = 25;
};

// Every external contract one AltSignalGateway instance is wired to. Defaults
// reproduce the deployed AltSignalQuality -> external-consumer integration
// exactly, so an absent `wiring:` section keeps production behaviour.
struct WiringConfig {
    std::string id = "vps1-boundary";

    // --- input: AltSignalQuality's output stream (externally owned) ----------
    // AltSignalQuality owns ALTSIGNALQUALITY_SIGNALS and publishes the FINAL
    // TradeSignal on signalquality.final.<token>.v1. The Gateway validates that
    // stream, never creates or reconfigures it, and owns only its own durable
    // consumer on it — exactly what that service's own docs prescribe for a
    // downstream consumer.
    std::string final_stream = "ALTSIGNALQUALITY_SIGNALS";
    std::string final_subject_filter = "signalquality.final.>";
    std::string final_durable = "signalgateway-final";
    // Delivery policy applied WHEN THE DURABLE IS FIRST CREATED. "new" (the
    // default) starts at the head: a FINAL signal published before this service
    // existed was never promised to anyone, and journalling it would create a
    // delivery obligation for a trade whose market closed weeks ago. "all"
    // replays the producer's whole retained stream and is the deliberate choice
    // when the journal is meant to hold the complete historical audit.
    // Ignored on every subsequent start: a restart resumes at the stored cursor.
    std::string final_deliver_policy = "new";   // "new" | "all"

    // --- upstream schemas read read-only over the same connection ------------
    // Both live in the shared AltSpotter database, so no FDW is involved: the
    // context snapshot and the instrument registry are plain local joins.
    std::string altstrategy_schema = "altstrategy";
    std::string altspotter_schema = "public";
    // AltTech's macro-core block, over a read-only postgres_fdw schema (the
    // AltTech database is separate). Read once per signal at ingest and on
    // extended-context queries; never on the delivery path.
    std::string alttech_schema = "alttech_ref";
    // AltSpotter exchange code the trade plan executes on. Used to resolve the
    // venue-executable symbol and the instrument spec.
    std::string execution_exchange = "bitget";
};

// Bounds on the protocol-2 query path. Every number here is a ceiling a query
// is refused beyond, never a value it is silently clamped to.
struct QueryConfig {
    int32_t default_page_size = 50;
    int32_t max_page_size = 200;
    // Per session, sliding one-minute window.
    int32_t rate_per_minute = 120;
    // The fixed candle windows an extended FULL context carries, per timeframe,
    // ending at the last bar closed by T0.
    int32_t candles_1m = 60;
    int32_t candles_5m = 48;
    int32_t candles_1h = 48;
    // A macro vector older than this at T0 is reported absent (MACRO_STALE),
    // not carried as if current. AltTech computes one every minute; a gap this
    // wide means the producer was down, and the consumer is told so.
    int32_t macro_max_age_sec = 900;
};

// The durable journal and its retention.
struct JournalConfig {
    int32_t postgres_pool_size = 3;
    // How many journal rows one delivery read may pull. Bounds RAM: the backlog
    // lives in PostgreSQL, never in the process.
    int32_t delivery_batch_size = 64;
    // Signals whose expires_at has passed and that were never delivered are
    // swept to EXPIRED by this loop rather than at read time, so the state in
    // the table is the truth rather than a predicate evaluated per query.
    int32_t expiry_sweep_interval_sec = 30;
    int32_t retention_interval_sec = 21600;
    // Retention of the journal itself. Delivered or not, a signal is an audit
    // record; see docs/JOURNAL.md for the storage arithmetic.
    int32_t retain_days = 400;
    // Rejected inbound messages (contract violations) are kept for far less
    // time: they are an operational signal, not a trading record.
    int32_t rejected_retain_days = 90;
};

// The NATS ingest loop.
struct IngestConfig {
    int32_t fetch_batch = 32;
    int32_t fetch_timeout_ms = 2000;
    // A signal older than this at arrival is journalled and marked EXPIRED
    // without ever being offered as live. It is NOT dropped: the audit trail
    // must show that VPS1 produced it.
    int32_t max_signal_age_sec = 0;  // 0 = rely solely on expires_at
};

// TLS for the WebSocket listener.
//
// There is deliberately no `enabled: false`. This endpoint is VPS1's external
// boundary, the only thing that ever leaves the box is a trade instruction, and
// a switch that turns the transport security off is a switch someone eventually
// leaves off. Local development uses a throwaway self-signed certificate
// (scripts/gen-tls-cert.sh), which exercises the real code path instead of
// bypassing it.
struct TlsConfig {
    std::string certificate_chain_file;
    std::string private_key_file;
    std::string dh_params_file;         // optional
    // Optional mutual TLS. When set, a client must present a certificate that
    // verifies against this CA bundle IN ADDITION to passing the HMAC
    // handshake. Defence in depth, off by default.
    bool require_client_certificate = false;
    std::string client_ca_file;
    // Minimum protocol version. TLS 1.2 is the floor; 1.3 is the default.
    std::string min_version = "1.3";     // "1.2" | "1.3"
    std::string ciphersuites_tls13;      // empty = OpenSSL default
    std::string cipher_list_tls12;       // empty = a hardened built-in list
};

// One authorised WebSocket client identity.
struct ClientCredential {
    std::string client_id;
    std::string key_id;
    std::string secret_hex;             // >= 32 bytes of key material, hex
    std::vector<std::string> capabilities;  // receive | ack | resume
    int32_t max_sessions = 1;
};

struct AuthConfig {
    // Absolute path to the credentials file (never the main config: it holds
    // key material and is mode 0640 root:altsignalgateway).
    std::string credentials_file;
    int32_t max_clock_skew_sec = 60;
    int32_t handshake_timeout_sec = 10;
    // Cool-down applied to a peer address after repeated authentication
    // failures. Bounds an offline brute force against the HMAC keys.
    int32_t auth_failure_threshold = 5;
    int32_t auth_failure_window_sec = 60;
    int32_t auth_lockout_sec = 300;
};

struct WebSocketConfig {
    std::string listen_address = "0.0.0.0";
    uint16_t listen_port = 9443;
    int32_t io_threads = 2;
    int32_t max_clients = 8;
    // The inbound message bound, enforced by the WebSocket transport BEFORE any
    // buffering (Beast's read_message_max). There is deliberately no second
    // frame-level setting: Beast reassembles a fragmented message and refuses it
    // at this bound, so a "max frame size" knob would have measured the same
    // quantity twice and read like a protection that does not exist.
    uint64_t max_message_bytes = 1 * 1024 * 1024;
    // Per-session outbound bounds. Exceeding them disconnects the session; the
    // journal keeps the backlog and RESUME replays it.
    int32_t max_queued_messages = 256;
    uint64_t max_queued_bytes = 16 * 1024 * 1024;
    int32_t heartbeat_interval_sec = 15;
    int32_t heartbeat_timeout_sec = 45;
    int32_t resume_timeout_sec = 30;
    // How long a SENT-but-unacked message may sit before the session is
    // considered non-compliant and closed (the journal then redelivers).
    int32_t ack_timeout_sec = 120;
    int32_t shutdown_drain_ms = 2000;
    // Backlog for the listening socket.
    int32_t accept_backlog = 16;
};

// The unified service configuration (/etc/altsignalgateway/altsignalgateway.yaml).
struct RuntimeConfig {
    CommonConfig common;
    WiringConfig wiring;
    JournalConfig journal;
    IngestConfig ingest;
    TlsConfig tls;
    AuthConfig auth;
    WebSocketConfig websocket;
    QueryConfig query;
    YAML::Node root;  // env-expanded document
};

Result<RuntimeConfig> load_runtime(const std::string& path);

// Load and validate the client credentials file. Enforced here rather than at
// first use: a malformed or world-readable credentials file must stop the
// service at startup, not at the first connection attempt at 3am.
Result<std::vector<ClientCredential>> load_credentials(const std::string& path);

// --- field helpers (collect readable errors instead of YAML exceptions) ---
Result<std::string> req_str(const YAML::Node& n, const std::string& key);
Result<int64_t> req_int(const YAML::Node& n, const std::string& key, int64_t min_v, int64_t max_v);
std::string opt_str(const YAML::Node& n, const std::string& key, const std::string& def);
int64_t opt_int(const YAML::Node& n, const std::string& key, int64_t def);
double opt_double(const YAML::Node& n, const std::string& key, double def);
bool opt_bool(const YAML::Node& n, const std::string& key, bool def);

} // namespace altsignalgateway::config
