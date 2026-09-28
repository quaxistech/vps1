#include "altmacro/core/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <unistd.h>
#include <string>

namespace {

std::string write_temp(const std::string& body) {
    const std::string path = "/tmp/altmacro_test_config_" + std::to_string(::getpid()) +
                             "_" + std::to_string(std::rand()) + ".yaml";
    std::ofstream out(path);
    out << body;
    return path;
}

const char* kMinimal = R"(
instance_id: altmacro-test
database:
  host: 127.0.0.1
  database: altmacro
bootstrap:
  history_start: "2025-12-01T00:00:00Z"
instruments:
  - code: FX.USDCNH
)";

} // namespace

TEST_CASE("a minimal configuration loads", "[config]") {
    const auto path = write_temp(kMinimal);
    auto cfg = altmacro::Config::load(path);
    std::remove(path.c_str());
    REQUIRE(cfg.has_value());
    CHECK(cfg->instance_id == "altmacro-test");
    CHECK(cfg->instruments.size() == 1);
    // Defaults must describe the contract the service actually speaks: a stale
    // default once cost this stack a silently narrowed subscription.
    CHECK(cfg->nats.stream == "ALTMACRO_SERIES");
    CHECK(cfg->nats.subject_prefix == "altmacro");
    CHECK(cfg->bootstrap.history_start == "2025-12-01T00:00:00Z");
}

TEST_CASE("a password in configuration is refused", "[config][security]") {
    // Accepting it would put the secret into every backup, diff and deployment
    // log from then on.
    const auto path = write_temp(R"(
instance_id: altmacro-test
database:
  host: 127.0.0.1
  database: altmacro
  password: hunter2
bootstrap:
  history_start: "2025-12-01T00:00:00Z"
instruments:
  - code: FX.USDCNH
)");
    auto cfg = altmacro::Config::load(path);
    std::remove(path.c_str());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().code == altmacro::ErrorCode::config_error);
    // The refusal must not echo the value it refused.
    CHECK(cfg.error().message.find("hunter2") == std::string::npos);
}

TEST_CASE("a literal provider token is refused", "[config][security]") {
    const auto path = write_temp(R"(
instance_id: altmacro-test
database:
  host: 127.0.0.1
bootstrap:
  history_start: "2025-12-01T00:00:00Z"
instruments:
  - code: FX.USDCNH
providers:
  tiingo:
    token: abc123
)");
    auto cfg = altmacro::Config::load(path);
    std::remove(path.c_str());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().message.find("abc123") == std::string::npos);
}

TEST_CASE("a configuration with no instruments is refused", "[config]") {
    const auto path = write_temp("instance_id: x\n");
    auto cfg = altmacro::Config::load(path);
    std::remove(path.c_str());
    CHECK_FALSE(cfg.has_value());
}

TEST_CASE("the DSN carries no password when none is given", "[config][security]") {
    const auto path = write_temp(kMinimal);
    auto cfg = altmacro::Config::load(path);
    std::remove(path.c_str());
    REQUIRE(cfg.has_value());
    CHECK(cfg->dsn("").find("password") == std::string::npos);
    CHECK(cfg->dsn("s3cret").find("password='s3cret'") != std::string::npos);
    // The summary printed at startup must never contain it.
    CHECK(cfg->redacted_summary().find("s3cret") == std::string::npos);
    CHECK(cfg->redacted_summary().find("password") == std::string::npos);
}

TEST_CASE("a URL with a password is never logged in full", "[config][security]") {
    // This is a regression, not a precaution. The startup summary printed the
    // NATS URL verbatim and put a live credential into journald.
    const std::string url = "nats://altmacro:s3cr3tpassword@127.0.0.1:4222";
    const auto safe = altmacro::Config::redact_url_for_logging(url);
    CHECK(safe.find("s3cr3tpassword") == std::string::npos);
    // The account name stays: it says which identity is in use and is not secret.
    CHECK(safe.find("altmacro") != std::string::npos);
    CHECK(safe.find("127.0.0.1:4222") != std::string::npos);
}

TEST_CASE("a URL without credentials is left alone", "[config][security]") {
    const std::string url = "nats://127.0.0.1:4222";
    CHECK(altmacro::Config::redact_url_for_logging(url) == url);
}

TEST_CASE("the startup summary carries no credential", "[config][security]") {
    const auto path = write_temp(R"(
instance_id: altmacro-test
database:
  host: 127.0.0.1
nats:
  url: nats://altmacro:topsecret@127.0.0.1:4222
bootstrap:
  history_start: "2025-12-01T00:00:00Z"
instruments:
  - code: FX.USDCNH
)");
    auto cfg = altmacro::Config::load(path);
    std::remove(path.c_str());
    REQUIRE(cfg.has_value());
    CHECK(cfg->redacted_summary().find("topsecret") == std::string::npos);
}


TEST_CASE("a missing history_start is an error, not a built-in date", "[config]") {
    // This stack has been bitten once by defaults that described a contract the
    // system had outgrown: a key dropped from the YAML silently restored an old
    // subscription. AltSpotter's retention horizon moves, so a built-in window
    // start would be right today and wrong later, without a word in any log.
    const auto path = write_temp(R"(
instance_id: altmacro-test
database:
  host: 127.0.0.1
instruments:
  - code: FX.USDCNH
)");
    auto cfg = altmacro::Config::load(path);
    std::remove(path.c_str());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().message.find("history_start") != std::string::npos);
}
