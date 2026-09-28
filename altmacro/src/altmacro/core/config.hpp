#pragma once

#include "altmacro/core/error.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace altmacro {

struct DatabaseConfig {
    // DSN without a password. The password is read from the environment at
    // startup (ALTMACRO_DB_PASSWORD) and appended in memory only. It is never
    // written to configuration, never logged, and never appears in an error
    // message: connection failures report the host and database, not the DSN.
    std::string host = "127.0.0.1";
    int32_t port = 5432;
    std::string database = "altmacro";
    std::string user = "altmacro_app";
    int32_t pool_size = 4;
    int32_t statement_timeout_ms = 60000;
};

struct NatsConfig {
    std::string url = "nats://127.0.0.1:4222";
    std::string stream = "ALTMACRO_SERIES";
    // Subject shape follows the stack's existing nomenclature, which is
    // <service>.<kind>.<scope>: alttech publishes alttech.feature.venue.<...>
    // and alttech.quality.<...>, so this publishes altmacro.bar.<instrument>.
    // <basis> and altmacro.reference.<instrument>. Instrument codes carry dots
    // (FX.USDCNH), and a dot is NATS's token separator, so the code is written
    // with underscores inside a subject: FX_USDCNH.
    std::string subject_prefix = "altmacro";
    std::string credentials_file;   // path only; contents never read into a log
    int32_t publish_timeout_ms = 5000;
    bool enabled = true;
};

// One instrument the service is asked to carry. Everything semantic about it
// lives in the database registry; this is only the operational envelope.
struct InstrumentConfig {
    std::string code;              // FX.USDCNH
    std::vector<std::string> timeframes = {"1m"};
    bool enabled = true;
};

struct BootstrapConfig {
    // The target window. It is aligned to AltSpotter's real 1m history so that
    // external series cover the same ground as the crypto they will be studied
    // against. A source that does not reach this far back is recorded as
    // covering less; nothing is invented to fill the difference.
    //
    // Deliberately NO default. AltSpotter's retention horizon moves, so a
    // built-in date would be right today and quietly wrong later -- and this
    // stack has already been bitten once by defaults that described a contract
    // the system had since outgrown. A missing key is an error, not an
    // assumption.
    std::string history_start;
    // Empty means "up to now at the moment the bootstrap runs".
    std::string history_end;
    // Rows accumulated before a flush. Bounds memory: a bootstrap must never
    // hold its whole result set, and at 1m across four instruments and two
    // sides that set is millions of rows.
    int32_t batch_rows = 5000;
    // How many days a single fetch may cover for daily reference series.
    int32_t reference_chunk_days = 3650;
    bool resume = true;
};

struct PollConfig {
    // Intraday sources are archives: they are written after the fact, so the
    // poller re-reads recent days rather than pretending to stream.
    int32_t intraday_interval_sec = 900;
    int32_t intraday_lookback_hours = 6;
    int32_t reference_interval_sec = 3600;
    int32_t reference_lookback_days = 10;
};

struct ProviderConfig {
    bool enabled = true;
    // Present only for sources that need a secret. Names the environment
    // variable to read it from; the value itself is never stored here.
    std::string credential_env_var;
};

struct Config {
    std::string instance_id = "altmacro-1";
    std::string log_level = "info";
    // Sent to every provider. See core/http.hpp: FRED refuses a bare product
    // token, so the contact form is not optional in practice.
    std::string user_agent = "altmacro/1.0 (+https://altmacro.invalid)";
    DatabaseConfig database;
    NatsConfig nats;
    BootstrapConfig bootstrap;
    PollConfig poll;
    std::vector<InstrumentConfig> instruments;
    std::map<std::string, ProviderConfig> providers;
    int32_t watchdog_margin_sec = 30;

    [[nodiscard]] static Result<Config> load(const std::string& path);

    // Builds the libpq connection string. Takes the password as an argument so
    // that the caller reads it from the environment and it never lives in this
    // struct; the returned string is used once and not retained.
    [[nodiscard]] std::string dsn(const std::string& password) const;

    // Describes the configuration without any secret in it, for the startup
    // log. The NATS URL carries a password after environment expansion, so it
    // is redacted here rather than printed: an earlier version of this function
    // wrote the credential straight into journald.
    [[nodiscard]] std::string redacted_summary() const;

    // Exposed so the redaction itself can be tested.
    [[nodiscard]] static std::string redact_url_for_logging(const std::string& url);
};

} // namespace altmacro
