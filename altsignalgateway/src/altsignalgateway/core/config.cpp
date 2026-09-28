#include "altsignalgateway/core/config.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

namespace altsignalgateway::config {

namespace {

// Expand ${VAR} in a scalar string. An unset variable is an error: a secret
// that silently becomes the empty string is a service that starts with no
// authentication at all.
Result<std::string> expand_env(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
            const size_t end = in.find('}', i + 2);
            if (end == std::string::npos) {
                return fail(ErrorCode::config_error, "unterminated ${ in config value");
            }
            const std::string var = in.substr(i + 2, end - i - 2);
            const char* val = std::getenv(var.c_str());
            if (val == nullptr) {
                return fail(ErrorCode::config_error, "environment variable not set: " + var);
            }
            out += val;
            i = end + 1;
        } else {
            out.push_back(in[i]);
            ++i;
        }
    }
    return out;
}

Result<void> expand_node(YAML::Node node) {
    if (node.IsScalar()) {
        const std::string& raw = node.Scalar();
        if (raw.find("${") != std::string::npos) {
            auto expanded = expand_env(raw);
            if (!expanded) return std::unexpected(expanded.error());
            node = *expanded;
        }
        return {};
    }
    if (node.IsMap()) {
        for (auto it : node) {
            if (auto r = expand_node(it.second); !r) return r;
        }
    } else if (node.IsSequence()) {
        for (auto it : node) {
            if (auto r = expand_node(it); !r) return r;
        }
    }
    return {};
}

bool is_hex(std::string_view s) {
    if (s.empty() || (s.size() % 2) != 0) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

} // namespace

Result<YAML::Node> load_yaml(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return fail(ErrorCode::config_error, "config file not found: " + path);
    }
    try {
        YAML::Node node = YAML::LoadFile(path);
        if (auto r = expand_node(node); !r) {
            return fail(r.error().code, path + ": " + r.error().message);
        }
        return node;
    } catch (const std::exception& e) {
        return fail(ErrorCode::config_error, path + ": YAML parse error: " + e.what());
    }
}

Result<std::string> req_str(const YAML::Node& n, const std::string& key) {
    if (!n[key] || !n[key].IsScalar() || n[key].Scalar().empty()) {
        return fail(ErrorCode::config_error, "missing required config key: " + key);
    }
    return n[key].as<std::string>();
}

Result<int64_t> req_int(const YAML::Node& n, const std::string& key, int64_t min_v, int64_t max_v) {
    if (!n[key] || !n[key].IsScalar()) {
        return fail(ErrorCode::config_error, "missing required config key: " + key);
    }
    try {
        const int64_t v = n[key].as<int64_t>();
        if (v < min_v || v > max_v) {
            return fail(ErrorCode::config_error,
                        "config key " + key + " out of range [" + std::to_string(min_v) + ", " +
                            std::to_string(max_v) + "]: " + std::to_string(v));
        }
        return v;
    } catch (const std::exception&) {
        return fail(ErrorCode::config_error, "config key is not an integer: " + key);
    }
}

std::string opt_str(const YAML::Node& n, const std::string& key, const std::string& def) {
    if (n[key] && n[key].IsScalar()) return n[key].as<std::string>();
    return def;
}

int64_t opt_int(const YAML::Node& n, const std::string& key, int64_t def) {
    if (n[key] && n[key].IsScalar()) {
        try {
            return n[key].as<int64_t>();
        } catch (const std::exception&) {
        }
    }
    return def;
}

double opt_double(const YAML::Node& n, const std::string& key, double def) {
    if (n[key] && n[key].IsScalar()) {
        try {
            return n[key].as<double>();
        } catch (const std::exception&) {
        }
    }
    return def;
}

bool opt_bool(const YAML::Node& n, const std::string& key, bool def) {
    if (n[key] && n[key].IsScalar()) {
        try {
            return n[key].as<bool>();
        } catch (const std::exception&) {
        }
    }
    return def;
}

Result<RuntimeConfig> load_runtime(const std::string& path) {
    auto doc = load_yaml(path);
    if (!doc) return std::unexpected(doc.error());
    const YAML::Node& root = *doc;

    RuntimeConfig rt;
    rt.root = root;

    // --- common --------------------------------------------------------------
    auto env = req_str(root, "environment");
    if (!env) return std::unexpected(env.error());
    auto nats = req_str(root, "nats_url");
    if (!nats) return std::unexpected(nats.error());
    auto dsn = req_str(root, "postgres_dsn");
    if (!dsn) return std::unexpected(dsn.error());

    rt.common.environment = *env;
    rt.common.nats_url = *nats;
    rt.common.postgres_dsn = *dsn;
    rt.common.log_level = opt_str(root, "log_level", "info");
    rt.common.timezone = opt_str(root, "timezone", "UTC");
    rt.common.instance_id = opt_str(root, "instance_id", "altsignalgateway-1");
    rt.common.shutdown_timeout_sec =
        static_cast<int32_t>(opt_int(root, "shutdown_timeout_sec", 25));
    if (rt.common.timezone != "UTC") {
        return fail(ErrorCode::config_error,
                    "timezone must be UTC: every timestamp on this stack is UTC and a local "
                    "zone would silently shift point-in-time semantics");
    }

    // --- wiring --------------------------------------------------------------
    if (const YAML::Node w = root["wiring"]; w && w.IsMap()) {
        rt.wiring.id = opt_str(w, "id", rt.wiring.id);
        if (const YAML::Node in = w["input"]; in && in.IsMap()) {
            rt.wiring.final_stream = opt_str(in, "stream", rt.wiring.final_stream);
            rt.wiring.final_subject_filter =
                opt_str(in, "subject_filter", rt.wiring.final_subject_filter);
            rt.wiring.final_durable = opt_str(in, "durable", rt.wiring.final_durable);
            rt.wiring.final_deliver_policy =
                opt_str(in, "deliver", rt.wiring.final_deliver_policy);
        }
        rt.wiring.altstrategy_schema =
            opt_str(w, "altstrategy_schema", rt.wiring.altstrategy_schema);
        rt.wiring.altspotter_schema =
            opt_str(w, "altspotter_schema", rt.wiring.altspotter_schema);
        rt.wiring.alttech_schema = opt_str(w, "alttech_schema", rt.wiring.alttech_schema);
        rt.wiring.execution_exchange =
            opt_str(w, "execution_exchange", rt.wiring.execution_exchange);
    }
    if (rt.wiring.final_deliver_policy != "new" && rt.wiring.final_deliver_policy != "all") {
        return fail(ErrorCode::config_error,
                    "wiring.input.deliver must be \"new\" or \"all\", got \"" +
                        rt.wiring.final_deliver_policy + "\"");
    }
    // Schema names are interpolated into SQL identifiers. Anything outside the
    // unquoted-identifier character set is refused rather than escaped: this
    // config comes from a root-owned file, but a service that builds SQL from a
    // string must still be provably injection-free.
    for (const auto* schema : {&rt.wiring.altstrategy_schema, &rt.wiring.altspotter_schema,
                               &rt.wiring.alttech_schema}) {
        if (schema->empty() ||
            !std::all_of(schema->begin(), schema->end(), [](unsigned char c) {
                return std::isalnum(c) != 0 || c == '_';
            })) {
            return fail(ErrorCode::config_error,
                        "schema name must be [A-Za-z0-9_]+: '" + *schema + "'");
        }
    }

    // --- journal -------------------------------------------------------------
    if (const YAML::Node j = root["journal"]; j && j.IsMap()) {
        rt.journal.postgres_pool_size =
            static_cast<int32_t>(opt_int(j, "postgres_pool_size", rt.journal.postgres_pool_size));
        rt.journal.delivery_batch_size =
            static_cast<int32_t>(opt_int(j, "delivery_batch_size", rt.journal.delivery_batch_size));
        rt.journal.expiry_sweep_interval_sec = static_cast<int32_t>(
            opt_int(j, "expiry_sweep_interval_sec", rt.journal.expiry_sweep_interval_sec));
        rt.journal.retention_interval_sec = static_cast<int32_t>(
            opt_int(j, "retention_interval_sec", rt.journal.retention_interval_sec));
        rt.journal.retain_days =
            static_cast<int32_t>(opt_int(j, "retain_days", rt.journal.retain_days));
        rt.journal.rejected_retain_days = static_cast<int32_t>(
            opt_int(j, "rejected_retain_days", rt.journal.rejected_retain_days));
    }
    if (rt.journal.postgres_pool_size < 2) {
        return fail(ErrorCode::config_error,
                    "journal.postgres_pool_size must be >= 2: the ingest loop and the delivery "
                    "pump each hold a connection and would deadlock on a pool of one");
    }
    if (rt.journal.delivery_batch_size < 1 || rt.journal.delivery_batch_size > 10000) {
        return fail(ErrorCode::config_error, "journal.delivery_batch_size out of range [1,10000]");
    }
    if (rt.journal.retain_days < 1) {
        return fail(ErrorCode::config_error, "journal.retain_days must be >= 1");
    }

    // --- ingest --------------------------------------------------------------
    if (const YAML::Node i = root["ingest"]; i && i.IsMap()) {
        rt.ingest.fetch_batch = static_cast<int32_t>(opt_int(i, "fetch_batch", rt.ingest.fetch_batch));
        rt.ingest.fetch_timeout_ms =
            static_cast<int32_t>(opt_int(i, "fetch_timeout_ms", rt.ingest.fetch_timeout_ms));
        rt.ingest.max_signal_age_sec =
            static_cast<int32_t>(opt_int(i, "max_signal_age_sec", rt.ingest.max_signal_age_sec));
    }
    if (rt.ingest.fetch_batch < 1 || rt.ingest.fetch_batch > 4096) {
        return fail(ErrorCode::config_error, "ingest.fetch_batch out of range [1,4096]");
    }

    // --- tls -----------------------------------------------------------------
    if (const YAML::Node t = root["tls"]; t && t.IsMap()) {
        rt.tls.certificate_chain_file = opt_str(t, "certificate_chain_file", "");
        rt.tls.private_key_file = opt_str(t, "private_key_file", "");
        rt.tls.dh_params_file = opt_str(t, "dh_params_file", "");
        rt.tls.require_client_certificate =
            opt_bool(t, "require_client_certificate", rt.tls.require_client_certificate);
        rt.tls.client_ca_file = opt_str(t, "client_ca_file", "");
        rt.tls.min_version = opt_str(t, "min_version", rt.tls.min_version);
        rt.tls.ciphersuites_tls13 = opt_str(t, "ciphersuites_tls13", "");
        rt.tls.cipher_list_tls12 = opt_str(t, "cipher_list_tls12", "");
    }
    if (rt.tls.certificate_chain_file.empty() || rt.tls.private_key_file.empty()) {
        return fail(ErrorCode::config_error,
                    "tls.certificate_chain_file and tls.private_key_file are required: this "
                    "endpoint has no plaintext mode");
    }
    if (rt.tls.require_client_certificate && rt.tls.client_ca_file.empty()) {
        return fail(ErrorCode::config_error,
                    "tls.client_ca_file is required while tls.require_client_certificate is "
                    "true: verifying against an empty trust store accepts nothing and silently "
                    "locks every client out");
    }
    if (rt.tls.min_version != "1.2" && rt.tls.min_version != "1.3") {
        return fail(ErrorCode::config_error, "tls.min_version must be \"1.2\" or \"1.3\"");
    }

    // --- auth ----------------------------------------------------------------
    if (const YAML::Node a = root["auth"]; a && a.IsMap()) {
        rt.auth.credentials_file = opt_str(a, "credentials_file", "");
        rt.auth.max_clock_skew_sec =
            static_cast<int32_t>(opt_int(a, "max_clock_skew_sec", rt.auth.max_clock_skew_sec));
        rt.auth.handshake_timeout_sec =
            static_cast<int32_t>(opt_int(a, "handshake_timeout_sec", rt.auth.handshake_timeout_sec));
        rt.auth.auth_failure_threshold = static_cast<int32_t>(
            opt_int(a, "auth_failure_threshold", rt.auth.auth_failure_threshold));
        rt.auth.auth_failure_window_sec = static_cast<int32_t>(
            opt_int(a, "auth_failure_window_sec", rt.auth.auth_failure_window_sec));
        rt.auth.auth_lockout_sec =
            static_cast<int32_t>(opt_int(a, "auth_lockout_sec", rt.auth.auth_lockout_sec));
    }
    if (rt.auth.credentials_file.empty()) {
        return fail(ErrorCode::config_error,
                    "auth.credentials_file is required: the WebSocket endpoint has no anonymous "
                    "mode");
    }
    if (rt.auth.max_clock_skew_sec < 1 || rt.auth.max_clock_skew_sec > 3600) {
        return fail(ErrorCode::config_error, "auth.max_clock_skew_sec out of range [1,3600]");
    }

    // --- websocket -----------------------------------------------------------
    if (const YAML::Node ws = root["websocket"]; ws && ws.IsMap()) {
        rt.websocket.listen_address = opt_str(ws, "listen_address", rt.websocket.listen_address);
        const int64_t port = opt_int(ws, "listen_port", rt.websocket.listen_port);
        if (port < 1 || port > 65535) {
            return fail(ErrorCode::config_error, "websocket.listen_port out of range [1,65535]");
        }
        rt.websocket.listen_port = static_cast<uint16_t>(port);
        rt.websocket.io_threads = static_cast<int32_t>(opt_int(ws, "io_threads", rt.websocket.io_threads));
        rt.websocket.max_clients =
            static_cast<int32_t>(opt_int(ws, "max_clients", rt.websocket.max_clients));
        if (ws["max_frame_bytes"]) {
            // Removed rather than silently ignored: a setting that is present in
            // a config file and does nothing is exactly the kind of thing an
            // operator later believes protected them.
            return fail(ErrorCode::config_error,
                        "websocket.max_frame_bytes no longer exists; the inbound bound is "
                        "websocket.max_message_bytes, enforced by the transport before "
                        "buffering");
        }
        rt.websocket.max_message_bytes = static_cast<uint64_t>(
            opt_int(ws, "max_message_bytes", static_cast<int64_t>(rt.websocket.max_message_bytes)));
        rt.websocket.max_queued_messages =
            static_cast<int32_t>(opt_int(ws, "max_queued_messages", rt.websocket.max_queued_messages));
        rt.websocket.max_queued_bytes = static_cast<uint64_t>(
            opt_int(ws, "max_queued_bytes", static_cast<int64_t>(rt.websocket.max_queued_bytes)));
        rt.websocket.heartbeat_interval_sec = static_cast<int32_t>(
            opt_int(ws, "heartbeat_interval_sec", rt.websocket.heartbeat_interval_sec));
        rt.websocket.heartbeat_timeout_sec = static_cast<int32_t>(
            opt_int(ws, "heartbeat_timeout_sec", rt.websocket.heartbeat_timeout_sec));
        rt.websocket.resume_timeout_sec =
            static_cast<int32_t>(opt_int(ws, "resume_timeout_sec", rt.websocket.resume_timeout_sec));
        rt.websocket.ack_timeout_sec =
            static_cast<int32_t>(opt_int(ws, "ack_timeout_sec", rt.websocket.ack_timeout_sec));
        rt.websocket.shutdown_drain_ms =
            static_cast<int32_t>(opt_int(ws, "shutdown_drain_ms", rt.websocket.shutdown_drain_ms));
        rt.websocket.accept_backlog =
            static_cast<int32_t>(opt_int(ws, "accept_backlog", rt.websocket.accept_backlog));
    }
    if (const YAML::Node q = rt.root["query"]; q && q.IsMap()) {
        rt.query.default_page_size =
            static_cast<int32_t>(opt_int(q, "default_page_size", rt.query.default_page_size));
        rt.query.max_page_size =
            static_cast<int32_t>(opt_int(q, "max_page_size", rt.query.max_page_size));
        rt.query.rate_per_minute =
            static_cast<int32_t>(opt_int(q, "rate_per_minute", rt.query.rate_per_minute));
        rt.query.candles_1m = static_cast<int32_t>(opt_int(q, "candles_1m", rt.query.candles_1m));
        rt.query.candles_5m = static_cast<int32_t>(opt_int(q, "candles_5m", rt.query.candles_5m));
        rt.query.candles_1h = static_cast<int32_t>(opt_int(q, "candles_1h", rt.query.candles_1h));
        rt.query.macro_max_age_sec =
            static_cast<int32_t>(opt_int(q, "macro_max_age_sec", rt.query.macro_max_age_sec));
    }
    if (rt.query.max_page_size < 1 || rt.query.max_page_size > 1000 ||
        rt.query.default_page_size < 1 || rt.query.default_page_size > rt.query.max_page_size) {
        return fail(ErrorCode::config_error,
                    "query.default_page_size must be in [1, query.max_page_size] and "
                    "query.max_page_size in [1,1000]");
    }
    if (rt.query.rate_per_minute < 1 || rt.query.rate_per_minute > 10000) {
        return fail(ErrorCode::config_error, "query.rate_per_minute out of range [1,10000]");
    }
    for (const int32_t n : {rt.query.candles_1m, rt.query.candles_5m, rt.query.candles_1h}) {
        if (n < 0 || n > 500) {
            return fail(ErrorCode::config_error, "query.candles_* out of range [0,500]");
        }
    }
    if (rt.query.macro_max_age_sec < 60 || rt.query.macro_max_age_sec > 86400) {
        return fail(ErrorCode::config_error, "query.macro_max_age_sec out of range [60,86400]");
    }
    if (rt.websocket.io_threads < 1 || rt.websocket.io_threads > 32) {
        return fail(ErrorCode::config_error, "websocket.io_threads out of range [1,32]");
    }
    if (rt.websocket.max_clients < 1 || rt.websocket.max_clients > 1024) {
        return fail(ErrorCode::config_error, "websocket.max_clients out of range [1,1024]");
    }
    if (rt.websocket.max_message_bytes < 64 * 1024 ||
        rt.websocket.max_message_bytes > 64ULL * 1024 * 1024) {
        return fail(ErrorCode::config_error,
                    "websocket.max_message_bytes out of range [65536, 67108864]");
    }
    if (rt.websocket.heartbeat_timeout_sec <= rt.websocket.heartbeat_interval_sec) {
        return fail(ErrorCode::config_error,
                    "websocket.heartbeat_timeout_sec must exceed heartbeat_interval_sec, "
                    "otherwise a healthy client is disconnected between two beats");
    }
    if (rt.websocket.max_queued_messages < 1 || rt.websocket.max_queued_messages > 100000) {
        return fail(ErrorCode::config_error, "websocket.max_queued_messages out of range [1,100000]");
    }
    return rt;
}

Result<std::vector<ClientCredential>> load_credentials(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return fail(ErrorCode::config_error, "credentials file not found: " + path);
    }
    // Key material readable by the world is a finding whether or not it has
    // been used yet. Refuse to start rather than log a warning nobody reads.
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return fail(ErrorCode::config_error, "cannot stat credentials file: " + path);
    }
    if ((st.st_mode & S_IROTH) != 0 || (st.st_mode & S_IWOTH) != 0) {
        return fail(ErrorCode::config_error,
                    "credentials file is world-accessible (mode " +
                        std::to_string(st.st_mode & 07777) + "): " + path +
                        " — expected 0640 or stricter");
    }

    auto doc = load_yaml(path);
    if (!doc) return std::unexpected(doc.error());
    const YAML::Node clients = (*doc)["clients"];
    if (!clients || !clients.IsSequence() || clients.size() == 0) {
        return fail(ErrorCode::config_error,
                    path + ": `clients:` must be a non-empty sequence");
    }

    static const std::set<std::string> kKnownCapabilities{"receive", "ack", "resume", "query"};
    std::vector<ClientCredential> out;
    std::set<std::string> seen_client_ids;
    std::set<std::string> seen_key_ids;
    for (const auto& node : clients) {
        ClientCredential c;
        auto cid = req_str(node, "client_id");
        if (!cid) return std::unexpected(cid.error());
        auto kid = req_str(node, "key_id");
        if (!kid) return std::unexpected(kid.error());
        auto sec = req_str(node, "secret_hex");
        if (!sec) return std::unexpected(sec.error());
        c.client_id = *cid;
        c.key_id = *kid;
        c.secret_hex = *sec;
        c.max_sessions = static_cast<int32_t>(opt_int(node, "max_sessions", 1));

        if (!is_hex(c.secret_hex) || c.secret_hex.size() < 64) {
            return fail(ErrorCode::config_error,
                        "client '" + c.client_id +
                            "': secret_hex must be an even-length hex string of at least 32 "
                            "bytes (64 hex characters)");
        }
        if (!seen_client_ids.insert(c.client_id).second) {
            return fail(ErrorCode::config_error, "duplicate client_id: " + c.client_id);
        }
        // key_id is the lookup key on an inbound Auth frame; two clients sharing
        // one would make the identity behind a signature ambiguous.
        if (!seen_key_ids.insert(c.key_id).second) {
            return fail(ErrorCode::config_error, "duplicate key_id: " + c.key_id);
        }

        if (const YAML::Node caps = node["capabilities"]; caps && caps.IsSequence()) {
            for (const auto& cap : caps) {
                const auto name = cap.as<std::string>();
                if (!kKnownCapabilities.contains(name)) {
                    return fail(ErrorCode::config_error,
                                "client '" + c.client_id + "': unknown capability '" + name +
                                    "' (known: receive, ack, resume)");
                }
                c.capabilities.push_back(name);
            }
        }
        if (c.capabilities.empty()) {
            return fail(ErrorCode::config_error,
                        "client '" + c.client_id +
                            "': capabilities must be listed explicitly; a credential that grants "
                            "nothing is a configuration mistake, not a read-only client");
        }
        if (c.max_sessions < 1 || c.max_sessions > 16) {
            return fail(ErrorCode::config_error,
                        "client '" + c.client_id + "': max_sessions out of range [1,16]");
        }
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace altsignalgateway::config
