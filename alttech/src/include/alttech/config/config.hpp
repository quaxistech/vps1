#pragma once
// Internal AltTech configuration API.

#include "alttech/common/error.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <string>

namespace alttech::config {

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
};

Result<CommonConfig> load_common(const std::string& path);

// --- field helpers (collect readable errors instead of YAML exceptions) ---
Result<std::string> req_str(const YAML::Node& n, const std::string& key);
std::string opt_str(const YAML::Node& n, const std::string& key, const std::string& def);
int64_t opt_int(const YAML::Node& n, const std::string& key, int64_t def);
double opt_double(const YAML::Node& n, const std::string& key, double def);
bool opt_bool(const YAML::Node& n, const std::string& key, bool def);

} // namespace alttech::config
