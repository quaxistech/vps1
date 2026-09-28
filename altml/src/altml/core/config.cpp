#include "altml/core/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace altml::config {

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

std::vector<std::string> opt_str_list(const YAML::Node& n, const std::string& key) {
    std::vector<std::string> out;
    if (!n || !n[key] || !n[key].IsSequence()) return out;
    for (const auto& item : n[key]) {
        if (item.IsScalar()) out.push_back(item.as<std::string>());
    }
    return out;
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

Result<ProjectConfig> parse_project(const YAML::Node& node) {
    ProjectConfig p;  // defaults = the deployed Bitget integration
    if (!node) return p;
    if (!node.IsMap()) {
        return fail(ErrorCode::config_error, "'project' must be a mapping");
    }
    p.id = opt_str(node, "id", p.id);
    p.exchange = opt_str(node, "exchange", p.exchange);
    p.objective = opt_str(node, "objective", p.objective);
    if (const YAML::Node in = node["input"]; in && in.IsMap()) {
        p.input_stream = opt_str(in, "stream", p.input_stream);
        // Accepts either a single subject or a list of them, so an existing
        // one-line config keeps working unchanged.
        if (const YAML::Node f = in["subject_filter"]; f) {
            if (f.IsSequence()) {
                std::vector<std::string> filters;
                for (const auto& item : f) filters.push_back(item.as<std::string>());
                if (!filters.empty()) p.input_subject_filters = std::move(filters);
            } else {
                p.input_subject_filters = {f.as<std::string>()};
            }
        }
        p.durable = opt_str(in, "durable", p.durable);
    }
    if (const YAML::Node out = node["output"]; out && out.IsMap()) {
        p.output_stream = opt_str(out, "stream", p.output_stream);
        p.prediction_subject_prefix =
            opt_str(out, "prediction_subject_prefix", p.prediction_subject_prefix);
    }
    if (const YAML::Node ctl = node["control"]; ctl && ctl.IsMap()) {
        p.control_stream = opt_str(ctl, "stream", p.control_stream);
        if (const YAML::Node subs = ctl["subjects"]; subs && subs.IsSequence()) {
            p.control_subjects.clear();
            for (const auto& s : subs) p.control_subjects.push_back(s.as<std::string>());
        }
    }
    p.dataset_root = opt_str(node, "dataset_root", p.dataset_root);
    p.model_root = opt_str(node, "model_root", p.model_root);

    // Only the Bitget exchange mapping (alttech.v1 enum, fee model seed) is
    // implemented today. Reject anything else loudly instead of mislabeling.
    if (p.exchange != "bitget") {
        return fail(ErrorCode::config_error,
                    "project.exchange '" + p.exchange + "' is not supported yet (only 'bitget')");
    }
    if (p.id.empty()) {
        return fail(ErrorCode::config_error, "project.id must not be empty");
    }
    for (const auto* root : {&p.dataset_root, &p.model_root}) {
        if (root->empty() || (*root)[0] != '/') {
            return fail(ErrorCode::config_error, "project artifact roots must be absolute paths");
        }
    }
    if (p.prediction_subject_prefix.empty() || p.prediction_subject_prefix.back() == '.') {
        return fail(ErrorCode::config_error,
                    "project.output.prediction_subject_prefix must be a non-empty subject "
                    "without a trailing '.'");
    }
    if (p.input_stream.empty() || p.output_stream.empty() || p.control_stream.empty() ||
        p.durable.empty() || p.input_subject_filters.empty() || p.control_subjects.empty()) {
        return fail(ErrorCode::config_error, "project stream/subject names must not be empty");
    }
    return p;
}

} // namespace

Result<RuntimeConfig> load_runtime(const std::string& path) {
    auto doc = load_yaml(path);
    if (!doc) return std::unexpected(doc.error());
    auto common = load_common(path);
    if (!common) return std::unexpected(common.error());
    auto project = parse_project((*doc)["project"]);
    if (!project) return std::unexpected(project.error());

    RuntimeConfig rt;
    rt.common = std::move(*common);
    rt.project = std::move(*project);
    rt.root = *doc;
    return rt;
}

} // namespace altml::config
