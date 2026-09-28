// AltTech configuration implementation.
#include "alttech/config/config.hpp"

#include <cstdlib>
#include <filesystem>

namespace alttech::config {

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
    return c;
}

} // namespace alttech::config
