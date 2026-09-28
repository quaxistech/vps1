cmake_minimum_required(VERSION 3.28)

# altsignalgateway — VPS1's external FINAL TradeSignal boundary.
#
# One binary (altsignalgateway), one process, one systemd unit. It consumes
# AltSignalQuality's FINAL TradeSignal, validates it, canonicalizes it, writes
# it to a durable PostgreSQL journal with a persistent sequence and an integrity
# hash, and serves it over an authenticated TLS WebSocket with explicit
# acknowledgement and resume.
#
# It takes no trading decision, runs no model and computes no market data.
#
# Dependency policy is the stack's: system packages for everything Ubuntu 24.04
# ships, and exactly one pinned FetchContent dependency (nats.c) at the same
# revision the sibling services use, so the whole stack speaks to JetStream
# through one client. Boost.Beast supplies the WebSocket and TLS transport and
# comes from the distribution's libboost-dev — no vendored networking stack, no
# Python, no external service.
project(altsignalgateway VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(ALTSIGNALGATEWAY_BUILD_TESTS "Build unit and contract tests" ON)
option(ALTSIGNALGATEWAY_ENABLE_SANITIZERS "Enable ASan/UBSan" OFF)

if(ALTSIGNALGATEWAY_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(Boost 1.83 REQUIRED COMPONENTS system)
find_package(PkgConfig REQUIRED)
find_package(PostgreSQL REQUIRED)
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
target_link_libraries(PkgConfig::PQXX INTERFACE PostgreSQL::PostgreSQL)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

include(FetchContent)

set(NATS_BUILD_LIB_SHARED OFF CACHE INTERNAL "")
set(NATS_BUILD_LIB_STATIC ON CACHE INTERNAL "")
set(NATS_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(NATS_BUILD_STREAMING OFF CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
FetchContent_Declare(
    natsc
    URL https://github.com/nats-io/nats.c/archive/refs/tags/v3.13.0.tar.gz
    URL_HASH SHA256=f6ec9ee2ab367594b56dd3265e3561074ade7c3d7410a6f45a77704c5e537024
    EXCLUDE_FROM_ALL
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(natsc)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    add_compile_options(-Wno-maybe-uninitialized)
endif()

add_subdirectory(proto)

# --- the pure boundary logic -------------------------------------------------
# domain/ holds the contract validator, the canonicaliser, the delivery state
# machine, the protocol state machine and the authentication scheme. It links
# nothing but protobuf, hashing and OpenSSL's primitives, which is what makes
# every rule in docs/SIGNAL_CONTRACT.md and docs/WEBSOCKET_PROTOCOL.md testable
# without a database, a broker or a socket.
add_library(altsignalgateway_domain STATIC
    src/altsignalgateway/core/hash.cpp
    src/altsignalgateway/core/time.cpp
    src/altsignalgateway/domain/auth.cpp
    src/altsignalgateway/domain/canonical.cpp
    src/altsignalgateway/domain/delivery_state.cpp
    src/altsignalgateway/domain/ids.cpp
    src/altsignalgateway/domain/validation.cpp)
target_include_directories(altsignalgateway_domain PUBLIC src)
target_link_libraries(altsignalgateway_domain
    PUBLIC altsignalgateway_proto nlohmann_json::nlohmann_json
    PRIVATE OpenSSL::Crypto)

# --- shared runtime: config, logging, lifecycle, PostgreSQL, NATS, codec -----
add_library(altsignalgateway_core STATIC
    src/altsignalgateway/core/codec.cpp
    src/altsignalgateway/core/config.cpp
    src/altsignalgateway/core/logger.cpp
    src/altsignalgateway/core/nats_client.cpp
    src/altsignalgateway/core/nats_provision.cpp
    src/altsignalgateway/core/pg.cpp
    src/altsignalgateway/core/readiness.cpp
    src/altsignalgateway/core/shutdown.cpp
    src/altsignalgateway/data/store.cpp)
target_include_directories(altsignalgateway_core PUBLIC src)
target_link_libraries(altsignalgateway_core
    PUBLIC altsignalgateway_domain Threads::Threads yaml-cpp::yaml-cpp
           nlohmann_json::nlohmann_json PkgConfig::PQXX nats_static
    PRIVATE OpenSSL::Crypto spdlog::spdlog fmt::fmt PkgConfig::SYSTEMD)

# --- the WebSocket boundary --------------------------------------------------
add_library(altsignalgateway_ws STATIC
    src/altsignalgateway/components/delivery_pump.cpp
    src/altsignalgateway/components/query_service.cpp
    src/altsignalgateway/components/session.cpp
    src/altsignalgateway/components/session_registry.cpp
    src/altsignalgateway/components/ws_server.cpp)
target_include_directories(altsignalgateway_ws PUBLIC src)
target_link_libraries(altsignalgateway_ws
    PUBLIC altsignalgateway_core Boost::system OpenSSL::SSL OpenSSL::Crypto)
# Beast's header-only build is heavy; separate compilation keeps it out of every
# other translation unit.

# --- the service binary ------------------------------------------------------
add_executable(altsignalgateway
    src/main.cpp
    src/altsignalgateway/cli/admin.cpp
    src/altsignalgateway/cli/client.cpp
    src/altsignalgateway/cli/run.cpp
    src/altsignalgateway/cli/selftest.cpp
    src/altsignalgateway/components/ingestor.cpp
    src/altsignalgateway/components/supervisor.cpp)
target_compile_definitions(altsignalgateway
    PRIVATE ALTSIGNALGATEWAY_VERSION="${PROJECT_VERSION}")
target_link_libraries(altsignalgateway PRIVATE altsignalgateway_ws protobuf::libprotobuf)

if(ALTSIGNALGATEWAY_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 REQUIRED)
    add_subdirectory(tests)
endif()

install(TARGETS altsignalgateway RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(PROGRAMS
    scripts/provision.sh
    scripts/db-bootstrap.sh
    scripts/nats-user.sh
    scripts/gen-tls-cert.sh
    scripts/install-systemd-units.sh
    DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY db/migrations/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altsignalgateway/db/migrations"
    FILES_MATCHING PATTERN "*.sql")
install(DIRECTORY proto/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altsignalgateway/proto"
    FILES_MATCHING PATTERN "*.proto")
install(FILES config/altsignalgateway.example.yaml
              config/clients.example.yaml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altsignalgateway/config-examples")
install(FILES config/altsignalgateway.env.example
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altsignalgateway/config-examples"
    RENAME common.env.example)
install(DIRECTORY deploy/systemd/
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system"
    FILES_MATCHING PATTERN "*.service" PATTERN "*.conf")
install(FILES README.md docs/CONSUMER_GUIDE.md
              docs/ARCHITECTURE.md
              docs/SIGNAL_CONTRACT.md
              docs/WEBSOCKET_PROTOCOL.md
              docs/JOURNAL.md
              docs/DELIVERY.md
              docs/SECURITY.md
              docs/OPERATIONS.md
              docs/TESTING.md
              docs/CONFIGURATION.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
