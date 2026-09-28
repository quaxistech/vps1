#include "altmacro/core/config.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace altmacro {
namespace {

template <typename T>
T get_or(const YAML::Node& node, const char* key, T fallback) {
    if (!node || !node[key] || node[key].IsNull()) return fallback;
    return node[key].as<T>();
}

// Expand ${VAR} against the environment, as the sibling services' configs do.
//
// This is how a credential-bearing URL reaches the process without ever being
// written into a file that gets backed up, diffed or copied: the YAML holds the
// name of the variable, the systemd EnvironmentFile holds the value.
//
// An unset variable is an error rather than an empty string. Silently expanding
// to nothing would produce a connection attempt to a URL missing its
// credentials, and the failure would surface far from its cause.
std::string expand_env(const std::string& in, bool* ok, std::string* missing) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
            const auto close = in.find('}', i + 2);
            if (close == std::string::npos) { out.push_back(in[i++]); continue; }
            const std::string name = in.substr(i + 2, close - i - 2);
            const char* value = std::getenv(name.c_str());
            if (value == nullptr) {
                if (ok != nullptr) *ok = false;
                if (missing != nullptr && missing->empty()) *missing = name;
            } else {
                out += value;
            }
            i = close + 1;
        } else {
            out.push_back(in[i++]);
        }
    }
    return out;
}

// Strip credentials from a URL for logging.
//
// This exists because the obvious version of the startup summary printed the
// NATS URL verbatim, and after environment expansion that URL carries a
// password -- which went straight into journald. Anything that formats a URL
// for human consumption goes through here.
std::string redact_url(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return url;
    const auto at = url.find('@', scheme_end + 3);
    if (at == std::string::npos) return url;   // no userinfo, nothing to hide
    const auto user_end = url.find(':', scheme_end + 3);
    if (user_end != std::string::npos && user_end < at) {
        // Keep the username: it identifies which account is in use, which is
        // operationally useful and is not a secret.
        return url.substr(0, user_end + 1) + "<redacted>" + url.substr(at);
    }
    return url.substr(0, scheme_end + 3) + "<redacted>" + url.substr(at);
}

// Escape a libpq keyword/value element. Without this a host or database name
// containing a space or quote would silently truncate the DSN.
std::string pg_quote(const std::string& v) {
    std::string out = "'";
    for (char c : v) {
        if (c == '\'' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

} // namespace

Result<Config> Config::load(const std::string& path) {
    std::ifstream probe(path);
    if (!probe) {
        return fail(ErrorCode::config_error, "cannot open configuration file: " + path);
    }
    probe.close();

    Config c;
    try {
        const YAML::Node root = YAML::LoadFile(path);

        c.instance_id = get_or<std::string>(root, "instance_id", c.instance_id);
        c.log_level = get_or<std::string>(root, "log_level", c.log_level);
        c.user_agent = get_or<std::string>(root, "user_agent", c.user_agent);
        c.watchdog_margin_sec = get_or<int32_t>(root, "watchdog_margin_sec", c.watchdog_margin_sec);

        if (const auto db = root["database"]) {
            c.database.host = get_or<std::string>(db, "host", c.database.host);
            c.database.port = get_or<int32_t>(db, "port", c.database.port);
            c.database.database = get_or<std::string>(db, "database", c.database.database);
            c.database.user = get_or<std::string>(db, "user", c.database.user);
            c.database.pool_size = get_or<int32_t>(db, "pool_size", c.database.pool_size);
            c.database.statement_timeout_ms =
                get_or<int32_t>(db, "statement_timeout_ms", c.database.statement_timeout_ms);
            // A password in the file is a configuration error, not something to
            // quietly accept: it would then live in a backup, a diff and a
            // deployment log. Rejected loudly, without echoing the value.
            if (db["password"]) {
                return fail(ErrorCode::config_error,
                            "database.password must not appear in configuration; "
                            "the password is read from ALTMACRO_DB_PASSWORD at startup");
            }
        }

        if (const auto n = root["nats"]) {
            bool expanded = true;
            std::string missing;
            c.nats.url = expand_env(get_or<std::string>(n, "url", c.nats.url), &expanded, &missing);
            if (!expanded) {
                return fail(ErrorCode::config_error,
                            "nats.url references environment variable ${" + missing +
                                "}, which is not set");
            }
            c.nats.stream = get_or<std::string>(n, "stream", c.nats.stream);
            c.nats.subject_prefix = get_or<std::string>(n, "subject_prefix", c.nats.subject_prefix);
            c.nats.credentials_file =
                get_or<std::string>(n, "credentials_file", c.nats.credentials_file);
            c.nats.publish_timeout_ms =
                get_or<int32_t>(n, "publish_timeout_ms", c.nats.publish_timeout_ms);
            c.nats.enabled = get_or<bool>(n, "enabled", c.nats.enabled);
        }

        if (const auto b = root["bootstrap"]) {
            c.bootstrap.history_start = get_or<std::string>(b, "history_start", "");
            c.bootstrap.history_end = get_or<std::string>(b, "history_end", c.bootstrap.history_end);
            c.bootstrap.batch_rows = get_or<int32_t>(b, "batch_rows", c.bootstrap.batch_rows);
            c.bootstrap.reference_chunk_days =
                get_or<int32_t>(b, "reference_chunk_days", c.bootstrap.reference_chunk_days);
            c.bootstrap.resume = get_or<bool>(b, "resume", c.bootstrap.resume);
        }

        if (const auto p = root["poll"]) {
            c.poll.intraday_interval_sec =
                get_or<int32_t>(p, "intraday_interval_sec", c.poll.intraday_interval_sec);
            c.poll.intraday_lookback_hours =
                get_or<int32_t>(p, "intraday_lookback_hours", c.poll.intraday_lookback_hours);
            c.poll.reference_interval_sec =
                get_or<int32_t>(p, "reference_interval_sec", c.poll.reference_interval_sec);
            c.poll.reference_lookback_days =
                get_or<int32_t>(p, "reference_lookback_days", c.poll.reference_lookback_days);
        }

        if (const auto insts = root["instruments"]; insts && insts.IsSequence()) {
            for (const auto& node : insts) {
                InstrumentConfig ic;
                ic.code = get_or<std::string>(node, "code", "");
                if (ic.code.empty()) {
                    return fail(ErrorCode::config_error, "an instrument entry has no code");
                }
                ic.enabled = get_or<bool>(node, "enabled", true);
                if (node["timeframes"] && node["timeframes"].IsSequence()) {
                    ic.timeframes.clear();
                    for (const auto& tf : node["timeframes"]) {
                        ic.timeframes.push_back(tf.as<std::string>());
                    }
                }
                c.instruments.push_back(std::move(ic));
            }
        }

        if (const auto provs = root["providers"]; provs && provs.IsMap()) {
            for (const auto& kv : provs) {
                ProviderConfig pc;
                pc.enabled = get_or<bool>(kv.second, "enabled", true);
                pc.credential_env_var = get_or<std::string>(kv.second, "credential_env_var", "");
                // Same rule as the database: a literal token in configuration
                // is refused rather than tolerated.
                if (kv.second["token"] || kv.second["api_key"] || kv.second["secret"]) {
                    return fail(ErrorCode::config_error,
                                "provider '" + kv.first.as<std::string>() +
                                    "' carries a literal credential; use credential_env_var");
                }
                c.providers[kv.first.as<std::string>()] = std::move(pc);
            }
        }
    } catch (const YAML::Exception& e) {
        return fail(ErrorCode::config_error, std::string("YAML error in ") + path + ": " + e.what());
    }

    if (c.bootstrap.history_start.empty()) {
        return fail(ErrorCode::config_error,
                    "bootstrap.history_start is required and has no default: it tracks "
                    "AltSpotter's retention horizon, which moves, and a built-in date would "
                    "be right today and silently wrong later");
    }
    if (c.instruments.empty()) {
        return fail(ErrorCode::config_error, "no instruments configured; there would be nothing to do");
    }
    if (c.database.pool_size < 1) {
        return fail(ErrorCode::config_error, "database.pool_size must be at least 1");
    }
    if (c.bootstrap.batch_rows < 1) {
        return fail(ErrorCode::config_error, "bootstrap.batch_rows must be at least 1");
    }
    return c;
}

std::string Config::dsn(const std::string& password) const {
    std::ostringstream out;
    out << "host=" << pg_quote(database.host)
        << " port=" << database.port
        << " dbname=" << pg_quote(database.database)
        << " user=" << pg_quote(database.user);
    if (!password.empty()) out << " password=" << pg_quote(password);
    out << " application_name='altmacro'"
        << " options=" << pg_quote("-c statement_timeout=" +
                                   std::to_string(database.statement_timeout_ms));
    return out.str();
}

std::string Config::redact_url_for_logging(const std::string& url) { return redact_url(url); }

std::string Config::redacted_summary() const {
    std::ostringstream out;
    out << "instance=" << instance_id
        << " db=" << database.user << "@" << database.host << ":" << database.port
        << "/" << database.database
        << " pool=" << database.pool_size
        << " nats=" << (nats.enabled ? redact_url(nats.url) : std::string("disabled"))
        << " instruments=" << instruments.size()
        << " history_start=" << bootstrap.history_start;
    return out.str();
}

} // namespace altmacro
