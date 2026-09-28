#include "altsignalquality/core/config.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace altsignalquality::config {

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


namespace {

// A subject prefix is a NATS subject stem the service appends tokens to; an
// empty one or a trailing '.' would silently produce "..sym.v1".
Result<void> check_prefix(const std::string& value, const char* key) {
    if (value.empty() || value.back() == '.' || value.front() == '.') {
        return fail(ErrorCode::config_error,
                    std::string("wiring.") + key +
                        " must be a non-empty subject stem without a leading or trailing '.'");
    }
    return {};
}

Result<WiringConfig> parse_wiring(const YAML::Node& node) {
    WiringConfig w;  // defaults = the deployed AltStrategy -> VPS2 integration
    if (!node) return w;
    if (!node.IsMap()) {
        return fail(ErrorCode::config_error, "'wiring' must be a mapping");
    }
    w.id = opt_str(node, "id", w.id);
    w.objective = opt_str(node, "objective", w.objective);
    if (const YAML::Node in = node["input"]; in && in.IsMap()) {
        w.candidate_stream = opt_str(in, "stream", w.candidate_stream);
        w.candidate_subject_filter = opt_str(in, "subject_filter", w.candidate_subject_filter);
        w.candidate_durable = opt_str(in, "durable", w.candidate_durable);
    }
    if (const YAML::Node out = node["output"]; out && out.IsMap()) {
        w.output_stream = opt_str(out, "stream", w.output_stream);
        w.final_subject_prefix = opt_str(out, "final_subject_prefix", w.final_subject_prefix);
        w.evaluation_subject_prefix =
            opt_str(out, "evaluation_subject_prefix", w.evaluation_subject_prefix);
        w.model_event_subject_prefix =
            opt_str(out, "model_event_subject_prefix", w.model_event_subject_prefix);
    }
    w.altstrategy_schema = opt_str(node, "altstrategy_schema", w.altstrategy_schema);
    w.max_stop_distance_bps =
        opt_double(node, "max_stop_distance_bps", w.max_stop_distance_bps);
    w.model_root = opt_str(node, "model_root", w.model_root);

    if (w.id.empty() || w.objective.empty()) {
        return fail(ErrorCode::config_error, "wiring.id and wiring.objective must not be empty");
    }
    if (w.candidate_stream.empty() || w.output_stream.empty() ||
        w.candidate_durable.empty() || w.candidate_subject_filter.empty()) {
        return fail(ErrorCode::config_error, "wiring stream/subject names must not be empty");
    }
    for (const auto& [value, key] :
         {std::pair{&w.final_subject_prefix, "output.final_subject_prefix"},
          {&w.evaluation_subject_prefix, "output.evaluation_subject_prefix"},
          {&w.model_event_subject_prefix, "output.model_event_subject_prefix"}}) {
        if (auto r = check_prefix(*value, key); !r) return std::unexpected(r.error());
    }
    // Publishing under `signal.` would land this service's output inside
    // AltStrategy's ALTSTRATEGY_DECISIONS stream (bound to `signal.>`), making
    // two services share one stream's retention and consumer cursors.
    for (const auto* prefix : {&w.final_subject_prefix, &w.evaluation_subject_prefix,
                               &w.model_event_subject_prefix}) {
        if (prefix->rfind("signal.", 0) == 0) {
            return fail(ErrorCode::config_error,
                        "output subjects must not start with 'signal.': that root belongs to "
                        "AltStrategy's ALTSTRATEGY_DECISIONS stream");
        }
    }
    if (!(w.max_stop_distance_bps > 0.0)) {
        return fail(ErrorCode::config_error,
                    "wiring.max_stop_distance_bps must be positive");
    }
    if (w.model_root.empty() || w.model_root[0] != '/') {
        return fail(ErrorCode::config_error, "wiring.model_root must be an absolute path");
    }
    if (w.altstrategy_schema.empty() ||
        w.altstrategy_schema.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
            std::string::npos) {
        return fail(ErrorCode::config_error,
                    "wiring.altstrategy_schema must be a plain SQL identifier");
    }
    return w;
}

} // namespace

std::vector<std::string> WiringConfig::output_subject_roots() const {
    // The stream binds one wildcard per distinct first token of the owned
    // subjects, so a renamed prefix still lands in this service's own stream.
    std::vector<std::string> roots;
    for (const auto* prefix : {&final_subject_prefix, &evaluation_subject_prefix,
                               &model_event_subject_prefix}) {
        const auto dot = prefix->find('.');
        std::string root = (dot == std::string::npos ? *prefix : prefix->substr(0, dot)) + ".>";
        if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
    }
    std::sort(roots.begin(), roots.end());
    return roots;
}

Result<RuntimeConfig> load_runtime(const std::string& path) {
    auto doc = load_yaml(path);
    if (!doc) return std::unexpected(doc.error());
    auto common = load_common(path);
    if (!common) return std::unexpected(common.error());
    auto wiring = parse_wiring((*doc)["wiring"]);
    if (!wiring) return std::unexpected(wiring.error());

    RuntimeConfig rt;
    rt.common = std::move(*common);
    rt.wiring = std::move(*wiring);
    rt.root = *doc;
    return rt;
}

} // namespace altsignalquality::config
