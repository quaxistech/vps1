#pragma once

#include "altsignalquality/core/error.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <string>
#include <vector>

namespace altsignalquality::config {

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

Result<CommonConfig> load_common(const std::string& path);

// Every external contract one AltSignalQuality instance is wired to. Defaults
// reproduce the deployed AltStrategy -> VPS2 integration exactly, so an absent
// `wiring:` section keeps production behaviour; wiring a second deployment is
// another config file, never another build.
struct WiringConfig {
    std::string id = "bitget-core";
    std::string objective = "binary:signal_quality";

    // --- input: AltStrategy's decision stream (externally owned) -------------
    // AltStrategy owns ALTSTRATEGY_DECISIONS and publishes the main plan
    // variant of every non-INVALID proposal on signal.proposed.<symbol>.v1.
    // AltSignalQuality validates that stream, never creates or updates it, and
    // owns only its own durable consumer on it.
    std::string candidate_stream = "ALTSTRATEGY_DECISIONS";
    std::string candidate_subject_filter = "signal.proposed.>";
    std::string candidate_durable = "signalquality-candidates";

    // --- output: owned and provisioned by altsignalquality -------------------
    // Deliberately OUTSIDE the `signal.>` namespace: that root is bound to
    // AltStrategy's stream, and publishing into another service's stream would
    // make this service's output that service's retention problem.
    std::string output_stream = "ALTSIGNALQUALITY_SIGNALS";
    std::string final_subject_prefix = "signalquality.final";        // + ".<symbol>.v1"
    std::string evaluation_subject_prefix = "signalquality.evaluation"; // + ".<symbol>.v1"
    std::string model_event_subject_prefix = "signalquality.model";  // + ".<event>.v1"

    // --- upstream schema read read-only over the same connection -------------
    std::string altstrategy_schema = "altstrategy";
    // Data-sanity bound on the history this service learns from, NOT a trading
    // rule. AltStrategy's own edge estimator applies the same cut: a trade
    // whose stop sat ~90% away from entry is evidence about a defect in a past
    // plan geometry, not about a strategy's edge, and this stack's archive
    // holds such rows in bulk. See docs/TRAINING.md.
    double max_stop_distance_bps = 1000.0;

    // --- artifact roots ------------------------------------------------------
    std::string model_root = "/var/lib/altsignalquality/models";

    [[nodiscard]] std::vector<std::string> output_subject_roots() const;
};

// The unified service configuration (/etc/altsignalquality/altsignalquality.yaml):
// common keys at the document root, the wiring adapter, and the per-component
// sections reachable through `root` (`evaluator:`, `dataset:`, `training:`,
// `model_registry:`, `policy:`).
struct RuntimeConfig {
    CommonConfig common;
    WiringConfig wiring;
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

} // namespace altsignalquality::config
