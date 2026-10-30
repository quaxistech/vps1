#pragma once

#include "altml/core/error.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <string>
#include <vector>

namespace altml::config {

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

// Project adapter: names every external contract one AltML instance serves.
// The defaults reproduce the deployed Bitget integration byte-for-byte, so an
// absent `project:` section keeps the current production behavior. Connecting
// another project means another config file, not another build.
struct ProjectConfig {
    std::string id = "bitget-core";
    std::string exchange = "bitget";  // exchanges.code filter + subject segment
    std::string objective = "direction-1m";
    // input (externally owned; validated, never created)
    std::string input_stream = "ALTTECH_ANALYTICS";
    // All four venues, every grid. Bitget is the TARGET venue; Binance, OKX and
    // Bybit are INFORMATION venues whose vectors are archived as evidence and
    // never substituted for the target's own. Cross-venue and microstructure
    // vectors live in sibling namespaces (alttech.feature.cross_venue.* and
    // alttech.microfeature.venue.*) and are deliberately NOT matched by this
    // pattern -- `venue` and `cross_venue` are different subject tokens.
    // Subjects this service consumes. A list, because AltTech publishes what
    // AltML reads under two different roots: alttech.feature.> (per-venue and
    // cross-venue vectors) and alttech.microfeature.venue.> (the execution
    // venue's book and tape). A single ">" over both would also drag in
    // structure, regime and quality messages this service does not decode.
    //
    // Both filters, because these defaults are what a config missing the key
    // silently falls back to. They used to describe the contract as it was
    // BEFORE the filter widening -- one root and the superseded durable below
    // -- so dropping `subject_filter` or `durable` from the YAML would have
    // reattached the service to the narrow contract and stopped the execution
    // venue's book/tape from arriving at all, which is the exact failure the
    // widening was made to fix. Defaults must state the shipped contract.
    std::vector<std::string> input_subject_filters = {"alttech.feature.>",
                                                      "alttech.microfeature.venue.>"};
    // Ordinary startup refuses filter drift. The explicit stopped-daemon
    // nats-setup window migration widens the known minute-only contract in
    // place, preserving this durable and its ACK frontier; no cursor reset.
    std::string durable = "inference-features-v4";
    // output (owned and provisioned by altml; consumers create durables on it)
    std::string output_stream = "ALTML_ANALYTICS";
    std::string prediction_subject_prefix = "prediction.bitget";  // + ".<sym>.1m.<ver>"
    // control (owned and provisioned by altml)
    std::string control_stream = "ALTML_CONTROL";
    std::vector<std::string> control_subjects = {"ml.>", "evt.model.>", "cmd.model.>"};
    // artifact roots
    std::string dataset_root = "/var/lib/altml/datasets";
    std::string model_root = "/var/lib/altml/models";
};

// The unified service configuration (/etc/altml/altml.yaml): common keys at
// the document root plus the project adapter and per-component sections
// (`inference:`, `label_builder:`, `model_registry:`, `jobs:`), reachable
// through `root`.
struct RuntimeConfig {
    CommonConfig common;
    ProjectConfig project;
    YAML::Node root;  // env-expanded document
};

Result<RuntimeConfig> load_runtime(const std::string& path);

// --- field helpers (collect readable errors instead of YAML exceptions) ---
Result<std::string> req_str(const YAML::Node& n, const std::string& key);
Result<int64_t> req_int(const YAML::Node& n, const std::string& key, int64_t min_v, int64_t max_v);
std::string opt_str(const YAML::Node& n, const std::string& key, const std::string& def);
int64_t opt_int(const YAML::Node& n, const std::string& key, int64_t def);
double opt_double(const YAML::Node& n, const std::string& key, double def);
bool opt_bool(const YAML::Node& n, const std::string& key, bool def);
// A YAML sequence of scalars. Absent or non-sequence yields an empty vector, so
// a caller can tell "not configured" from "configured empty" only by intent --
// callers here treat empty as "keep the compiled default".
std::vector<std::string> opt_str_list(const YAML::Node& n, const std::string& key);

} // namespace altml::config
