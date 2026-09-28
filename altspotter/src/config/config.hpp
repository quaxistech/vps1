#pragma once

#include "core/error.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <string>
#include <vector>

namespace altspotter::config {

// Load YAML file and expand ${ENV_VAR} placeholders in all scalar values.
// Missing environment variables are an error (secrets must be explicit).
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

Result<CommonConfig> load_common(const std::string& path);

// One component's slice of the unified `altspotter run` configuration: the shared
// common block, this component's YAML sub-node (already env-expanded), and the
// component name kept for logging and the Envelope `source_service` field.
struct ServiceContext {
    CommonConfig common;
    YAML::Node service;
    YAML::Node root;  // full unified config (cross-section access: catalog, retention)
    std::string service_name;
};

struct BitgetRestConfig {
    std::string rest_base_url = "https://api.bitget.com";
    std::string product_type = "USDT-FUTURES";
    int32_t connect_timeout_ms = 5000;
    int32_t request_timeout_ms = 10000;
    double rate_limit_per_second = 5.0;
    int32_t max_retries = 4;
    int32_t backoff_min_ms = 250;
    int32_t backoff_max_ms = 8000;
};

Result<BitgetRestConfig> parse_bitget_rest(const YAML::Node& node);

struct BitgetWsConfig {
    std::string ws_public_url = "wss://ws.bitget.com/v2/ws/public";
    std::string product_type = "USDT-FUTURES";
    int32_t connect_timeout_ms = 8000;
    int32_t ws_ping_interval_sec = 25;
    int32_t ws_pong_timeout_sec = 15;
    int32_t ws_rotation_interval_sec = 86400;
    int32_t max_channels_per_connection = 40;
    int32_t max_outgoing_messages_per_second = 8;
    int32_t subscribe_batch_size = 10;
    int32_t stale_stream_timeout_sec = 60;
    int32_t backoff_min_ms = 500;
    int32_t backoff_max_ms = 30000;
};

Result<BitgetWsConfig> parse_bitget_ws(const YAML::Node& node);

// --- field helpers (collect readable errors instead of YAML exceptions) ---
Result<std::string> req_str(const YAML::Node& n, const std::string& key);
Result<int64_t> req_int(const YAML::Node& n, const std::string& key, int64_t min_v, int64_t max_v);
std::string opt_str(const YAML::Node& n, const std::string& key, const std::string& def);
int64_t opt_int(const YAML::Node& n, const std::string& key, int64_t def);
double opt_double(const YAML::Node& n, const std::string& key, double def);
bool opt_bool(const YAML::Node& n, const std::string& key, bool def);

} // namespace altspotter::config
