#include "altstrategy/config/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace altstrategy::config {

namespace {

// Expand ${VAR} in a scalar string; unknown vars produce an error message via out param.
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

Result<CommonConfig> load_common(const std::string& path) {
    auto doc = load_yaml(path);
    if (!doc) return std::unexpected(doc.error());
    const YAML::Node& n = *doc;

    CommonConfig c;
    auto env = req_str(n, "environment");
    auto nats = req_str(n, "nats_url");
    auto dsn = req_str(n, "postgres_dsn");
    auto inst = req_str(n, "instance_id");
    for (const auto* r : {&env, &nats, &dsn, &inst}) {
        if (!*r) return std::unexpected(r->error());
    }
    c.environment = *env;
    c.nats_url = *nats;
    c.postgres_dsn = *dsn;
    c.instance_id = *inst;
    c.log_level = opt_str(n, "log_level", "info");
    c.timezone = opt_str(n, "timezone", "UTC");
    if (c.timezone != "UTC") {
        return fail(ErrorCode::config_error, "timezone must be UTC");
    }
    c.shutdown_timeout_sec = static_cast<int32_t>(opt_int(n, "shutdown_timeout_sec", 25));
    if (c.shutdown_timeout_sec < 1 || c.shutdown_timeout_sec > 300) {
        return fail(ErrorCode::config_error, "shutdown_timeout_sec out of range [1, 300]");
    }
    return c;
}

Result<BitgetRestConfig> parse_bitget_rest(const YAML::Node& node) {
    if (!node || !node.IsMap()) {
        return fail(ErrorCode::config_error, "missing 'bitget' config section");
    }
    BitgetRestConfig c;
    c.rest_base_url = opt_str(node, "rest_base_url", c.rest_base_url);
    c.product_type = opt_str(node, "product_type", c.product_type);
    c.connect_timeout_ms = static_cast<int32_t>(opt_int(node, "connect_timeout_ms", 5000));
    c.request_timeout_ms = static_cast<int32_t>(opt_int(node, "request_timeout_ms", 10000));
    c.rate_limit_per_second = opt_double(node, "rate_limit_per_second", 5.0);
    c.max_retries = static_cast<int32_t>(opt_int(node, "max_retries", 4));
    c.backoff_min_ms = static_cast<int32_t>(opt_int(node, "backoff_min_ms", 250));
    c.backoff_max_ms = static_cast<int32_t>(opt_int(node, "backoff_max_ms", 8000));

    if (!c.rest_base_url.starts_with("https://")) {
        return fail(ErrorCode::config_error, "rest_base_url must be https://");
    }
    if (c.connect_timeout_ms < 100 || c.connect_timeout_ms > 60000 ||
        c.request_timeout_ms < 100 || c.request_timeout_ms > 120000) {
        return fail(ErrorCode::config_error, "REST timeouts out of range");
    }
    if (c.rate_limit_per_second <= 0.0 || c.rate_limit_per_second > 20.0) {
        return fail(ErrorCode::config_error, "rate_limit_per_second out of range (0, 20]");
    }
    if (c.max_retries < 0 || c.max_retries > 10) {
        return fail(ErrorCode::config_error, "max_retries out of range [0, 10]");
    }
    if (c.backoff_min_ms < 10 || c.backoff_max_ms < c.backoff_min_ms || c.backoff_max_ms > 120000) {
        return fail(ErrorCode::config_error, "backoff range invalid");
    }
    return c;
}

Result<BitgetWsConfig> parse_bitget_ws(const YAML::Node& node) {
    if (!node || !node.IsMap()) {
        return fail(ErrorCode::config_error, "missing 'bitget' config section");
    }
    BitgetWsConfig c;
    c.ws_public_url = opt_str(node, "ws_public_url", c.ws_public_url);
    c.product_type = opt_str(node, "product_type", c.product_type);
    c.connect_timeout_ms = static_cast<int32_t>(opt_int(node, "connect_timeout_ms", 8000));
    c.ws_ping_interval_sec = static_cast<int32_t>(opt_int(node, "ws_ping_interval_sec", 25));
    c.ws_pong_timeout_sec = static_cast<int32_t>(opt_int(node, "ws_pong_timeout_sec", 15));
    c.ws_rotation_interval_sec = static_cast<int32_t>(opt_int(node, "ws_rotation_interval_sec", 86400));
    c.max_channels_per_connection =
        static_cast<int32_t>(opt_int(node, "max_channels_per_connection", 40));
    c.max_outgoing_messages_per_second =
        static_cast<int32_t>(opt_int(node, "max_outgoing_messages_per_second", 8));
    c.subscribe_batch_size = static_cast<int32_t>(opt_int(node, "subscribe_batch_size", 10));
    c.stale_stream_timeout_sec = static_cast<int32_t>(opt_int(node, "stale_stream_timeout_sec", 60));
    c.backoff_min_ms = static_cast<int32_t>(opt_int(node, "backoff_min_ms", 500));
    c.backoff_max_ms = static_cast<int32_t>(opt_int(node, "backoff_max_ms", 30000));

    if (!c.ws_public_url.starts_with("wss://")) {
        return fail(ErrorCode::config_error, "ws_public_url must be wss://");
    }
    if (c.ws_ping_interval_sec < 5 || c.ws_ping_interval_sec > 30) {
        return fail(ErrorCode::config_error, "ws_ping_interval_sec out of range [5, 30]");
    }
    if (c.ws_pong_timeout_sec < 1 || c.ws_pong_timeout_sec > 60) {
        return fail(ErrorCode::config_error, "ws_pong_timeout_sec out of range [1, 60]");
    }
    if (c.max_channels_per_connection < 2 || c.max_channels_per_connection > 500) {
        return fail(ErrorCode::config_error, "max_channels_per_connection out of range [2, 500]");
    }
    if (c.subscribe_batch_size < 1 || c.subscribe_batch_size > 50) {
        return fail(ErrorCode::config_error, "subscribe_batch_size out of range [1, 50]");
    }
    if (c.stale_stream_timeout_sec < 10 || c.stale_stream_timeout_sec > 3600) {
        return fail(ErrorCode::config_error, "stale_stream_timeout_sec out of range [10, 3600]");
    }
    return c;
}

} // namespace altstrategy::config
