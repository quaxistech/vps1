// Configuration and credential loading.
//
// Everything here is a startup-time refusal. The point of each case is that the
// failure it describes must stop the service at boot rather than surfacing as a
// runtime surprise: a world-readable key file, an unset secret that silently
// became the empty string, a heartbeat timeout shorter than its own interval.

#include "altsignalgateway/core/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace altsignalgateway;

namespace {

std::filesystem::path scratch() {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path() /
               ("asgw-cfg-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
    std::filesystem::create_directories(dir);
    return dir;
}

void write(const std::filesystem::path& p, const std::string& body,
           std::filesystem::perms mode = std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write |
                                         std::filesystem::perms::group_read) {
    std::ofstream out(p);
    out << body;
    out.close();
    std::filesystem::permissions(p, mode);
}

const char* kMinimalConfig = R"(
environment: test
log_level: info
timezone: UTC
instance_id: test-1
nats_url: nats://127.0.0.1:4222
postgres_dsn: postgresql://u@127.0.0.1/db
tls:
  certificate_chain_file: /tmp/x.crt
  private_key_file: /tmp/x.key
auth:
  credentials_file: /tmp/clients.yaml
)";

} // namespace

TEST_CASE("a minimal configuration loads with production defaults") {
    const auto dir = scratch();
    write(dir / "cfg.yaml", kMinimalConfig);
    auto cfg = config::load_runtime((dir / "cfg.yaml").string());
    REQUIRE(cfg.has_value());
    CHECK(cfg->wiring.final_stream == "ALTSIGNALQUALITY_SIGNALS");
    CHECK(cfg->wiring.final_subject_filter == "signalquality.final.>");
    CHECK(cfg->wiring.final_durable == "signalgateway-final");
    CHECK(cfg->wiring.final_deliver_policy == "new");
    CHECK(cfg->websocket.listen_port == 9443);
    CHECK(cfg->tls.min_version == "1.3");
    std::filesystem::remove_all(dir);
}

TEST_CASE("an unset environment variable is a startup error") {
    // The alternative — expanding it to "" — is a service that starts with an
    // empty database password or an empty client secret.
    const auto dir = scratch();
    write(dir / "cfg.yaml", std::string(kMinimalConfig) +
                                "\nsomething: ${ALTSIGNALGATEWAY_DEFINITELY_UNSET_XYZ}\n");
    auto cfg = config::load_runtime((dir / "cfg.yaml").string());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().message.find("ALTSIGNALGATEWAY_DEFINITELY_UNSET_XYZ") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("TLS material is mandatory") {
    const auto dir = scratch();
    write(dir / "cfg.yaml", R"(
environment: test
nats_url: nats://127.0.0.1:4222
postgres_dsn: postgresql://u@127.0.0.1/db
auth:
  credentials_file: /tmp/clients.yaml
)");
    auto cfg = config::load_runtime((dir / "cfg.yaml").string());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().message.find("no plaintext mode") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("mutual TLS without a trust store is refused") {
    // verify_peer against an empty CA bundle accepts nothing, which would
    // silently lock every client out while looking like a security improvement.
    const auto dir = scratch();
    write(dir / "cfg.yaml", R"(
environment: test
nats_url: nats://127.0.0.1:4222
postgres_dsn: postgresql://u@127.0.0.1/db
tls:
  certificate_chain_file: /tmp/x.crt
  private_key_file: /tmp/x.key
  require_client_certificate: true
auth:
  credentials_file: /tmp/clients.yaml
)");
    auto cfg = config::load_runtime((dir / "cfg.yaml").string());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().message.find("client_ca_file") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("authentication cannot be turned off by omission") {
    const auto dir = scratch();
    write(dir / "cfg.yaml", R"(
environment: test
nats_url: nats://127.0.0.1:4222
postgres_dsn: postgresql://u@127.0.0.1/db
tls:
  certificate_chain_file: /tmp/x.crt
  private_key_file: /tmp/x.key
)");
    auto cfg = config::load_runtime((dir / "cfg.yaml").string());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().message.find("no anonymous mode") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a heartbeat timeout inside its own interval is refused") {
    const auto dir = scratch();
    write(dir / "cfg.yaml", std::string(kMinimalConfig) + R"(
websocket:
  heartbeat_interval_sec: 30
  heartbeat_timeout_sec: 15
)");
    auto cfg = config::load_runtime((dir / "cfg.yaml").string());
    REQUIRE_FALSE(cfg.has_value());
    // Otherwise a perfectly healthy client is disconnected between two beats.
    CHECK(cfg.error().message.find("heartbeat_timeout_sec") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a pool of one would deadlock the two database threads") {
    const auto dir = scratch();
    write(dir / "cfg.yaml", std::string(kMinimalConfig) + R"(
journal:
  postgres_pool_size: 1
)");
    auto cfg = config::load_runtime((dir / "cfg.yaml").string());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().message.find("postgres_pool_size") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a non-UTC timezone is refused") {
    // Every timestamp on this stack is UTC; a local zone would silently shift
    // the point-in-time semantics of every signal.
    const auto dir = scratch();
    write(dir / "cfg.yaml", R"(
environment: test
timezone: Europe/Berlin
nats_url: nats://127.0.0.1:4222
postgres_dsn: postgresql://u@127.0.0.1/db
tls:
  certificate_chain_file: /tmp/x.crt
  private_key_file: /tmp/x.key
auth:
  credentials_file: /tmp/clients.yaml
)");
    auto cfg = config::load_runtime((dir / "cfg.yaml").string());
    REQUIRE_FALSE(cfg.has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE("a schema name that is not an identifier is refused") {
    // The schema is interpolated into SQL. It comes from a root-owned file, but
    // a service that builds SQL from a string must still be provably free of
    // injection.
    const auto dir = scratch();
    write(dir / "cfg.yaml", std::string(kMinimalConfig) + R"(
wiring:
  altstrategy_schema: "altstrategy; DROP SCHEMA public CASCADE"
)");
    auto cfg = config::load_runtime((dir / "cfg.yaml").string());
    REQUIRE_FALSE(cfg.has_value());
    CHECK(cfg.error().message.find("schema name") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("an unknown delivery policy is refused") {
    const auto dir = scratch();
    write(dir / "cfg.yaml", std::string(kMinimalConfig) + R"(
wiring:
  input:
    deliver: sometimes
)");
    auto cfg = config::load_runtime((dir / "cfg.yaml").string());
    REQUIRE_FALSE(cfg.has_value());
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// credentials
// ---------------------------------------------------------------------------

TEST_CASE("a well-formed credentials file loads") {
    const auto dir = scratch();
    write(dir / "clients.yaml", R"(
clients:
  - client_id: vps2-executor
    key_id: k1
    secret_hex: ")" + std::string(64, 'a') + R"("
    capabilities: [receive, ack, resume]
    max_sessions: 1
)");
    auto creds = config::load_credentials((dir / "clients.yaml").string());
    REQUIRE(creds.has_value());
    REQUIRE(creds->size() == 1);
    CHECK((*creds)[0].client_id == "vps2-executor");
    CHECK((*creds)[0].capabilities.size() == 3);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a world-readable credentials file stops the service") {
    const auto dir = scratch();
    write(dir / "clients.yaml", R"(
clients:
  - client_id: a
    key_id: k1
    secret_hex: ")" + std::string(64, 'a') + R"("
    capabilities: [receive]
)",
          std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
              std::filesystem::perms::group_read | std::filesystem::perms::others_read);
    auto creds = config::load_credentials((dir / "clients.yaml").string());
    REQUIRE_FALSE(creds.has_value());
    CHECK(creds.error().message.find("world-accessible") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("weak or malformed key material is refused") {
    const auto dir = scratch();
    SECTION("too short") {
        write(dir / "c.yaml", R"(
clients:
  - client_id: a
    key_id: k1
    secret_hex: "0011223344556677"
    capabilities: [receive]
)");
        CHECK_FALSE(config::load_credentials((dir / "c.yaml").string()).has_value());
    }
    SECTION("not hex") {
        write(dir / "c.yaml", R"(
clients:
  - client_id: a
    key_id: k1
    secret_hex: ")" + std::string(64, 'z') + R"("
    capabilities: [receive]
)");
        CHECK_FALSE(config::load_credentials((dir / "c.yaml").string()).has_value());
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("ambiguous identities are refused") {
    const auto dir = scratch();
    SECTION("two clients sharing a key id") {
        // key_id is what an AUTH frame selects, so a shared one would make the
        // identity behind a signature ambiguous.
        write(dir / "c.yaml", R"(
clients:
  - client_id: a
    key_id: shared
    secret_hex: ")" + std::string(64, 'a') + R"("
    capabilities: [receive]
  - client_id: b
    key_id: shared
    secret_hex: ")" + std::string(64, 'b') + R"("
    capabilities: [receive]
)");
        auto r = config::load_credentials((dir / "c.yaml").string());
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().message.find("duplicate key_id") != std::string::npos);
    }
    SECTION("a duplicated client id") {
        write(dir / "c.yaml", R"(
clients:
  - client_id: same
    key_id: k1
    secret_hex: ")" + std::string(64, 'a') + R"("
    capabilities: [receive]
  - client_id: same
    key_id: k2
    secret_hex: ")" + std::string(64, 'b') + R"("
    capabilities: [receive]
)");
        auto r = config::load_credentials((dir / "c.yaml").string());
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().message.find("duplicate client_id") != std::string::npos);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a credential must state what it may do") {
    const auto dir = scratch();
    SECTION("no capabilities at all") {
        write(dir / "c.yaml", R"(
clients:
  - client_id: a
    key_id: k1
    secret_hex: ")" + std::string(64, 'a') + R"("
)");
        CHECK_FALSE(config::load_credentials((dir / "c.yaml").string()).has_value());
    }
    SECTION("an invented capability") {
        write(dir / "c.yaml", R"(
clients:
  - client_id: a
    key_id: k1
    secret_hex: ")" + std::string(64, 'a') + R"("
    capabilities: [receive, admin]
)");
        auto r = config::load_credentials((dir / "c.yaml").string());
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().message.find("unknown capability") != std::string::npos);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("an empty client list is refused") {
    const auto dir = scratch();
    write(dir / "c.yaml", "clients: []\n");
    CHECK_FALSE(config::load_credentials((dir / "c.yaml").string()).has_value());
    std::filesystem::remove_all(dir);
}
