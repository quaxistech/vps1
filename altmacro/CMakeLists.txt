cmake_minimum_required(VERSION 3.28)

# altmacro — VPS1's external market and macro reference data service.
#
# One binary (altmacro), one process, one systemd unit. It fetches published
# external data, normalises timestamps and units, records where every number
# came from and when it became knowable, stores it in PostgreSQL and publishes
# it to JetStream.
#
# It states what a number IS. It does not state what a number MEANS: no
# correlation, no beta, no lead-lag, no z-score, no regime. Those belong to
# AltTech and AltML, which must be free to discover relationships this service
# has not pre-judged.
#
# AltSpotter remains authoritative for crypto. Nothing here duplicates it.
#
# Dependency policy is the stack's: system packages for everything Ubuntu 24.04
# ships, plus exactly one pinned FetchContent dependency (nats.c) at the same
# revision the sibling services use. Boost.Beast supplies HTTPS, as in
# AltSpotter's exchange client. liblzma decodes Dukascopy's .bi5 files and
# comes from the distribution.
project(altmacro VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(ALTMACRO_BUILD_TESTS "Build unit and contract tests" ON)
option(ALTMACRO_ENABLE_SANITIZERS "Enable ASan/UBSan" OFF)

if(ALTMACRO_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
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
pkg_check_modules(LZMA REQUIRED IMPORTED_TARGET liblzma)

include(FetchContent)

set(NATS_BUILD_LIB_SHARED OFF CACHE INTERNAL "")
set(NATS_BUILD_LIB_STATIC ON CACHE INTERNAL "")
set(NATS_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(NATS_BUILD_STREAMING OFF CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
FetchContent_Declare(
    natsc
    GIT_REPOSITORY https://github.com/nats-io/nats.c.git
    GIT_TAG v3.8.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(natsc)

add_library(altmacro_core STATIC
    src/altmacro/core/config.cpp
    src/altmacro/core/http.cpp
    src/altmacro/core/logger.cpp
    src/altmacro/core/pg.cpp
    src/altmacro/core/rate_limiter.cpp
    src/altmacro/core/readiness.cpp
    src/altmacro/core/shutdown.cpp
    src/altmacro/core/time.cpp
    src/altmacro/core/nats_client.cpp
    src/altmacro/core/nats_provision.cpp
    src/altmacro/domain/series.cpp
    src/altmacro/domain/calendar.cpp
    src/altmacro/domain/validation.cpp
    src/altmacro/data/store.cpp
    src/altmacro/providers/registry.cpp
    src/altmacro/providers/dukascopy.cpp
    src/altmacro/providers/fred.cpp
    src/altmacro/providers/frankfurter.cpp
    src/altmacro/components/bootstrap.cpp
    src/altmacro/components/poller.cpp
    src/altmacro/components/publisher.cpp
    src/altmacro/components/gap_detector.cpp
    src/altmacro/components/supervisor.cpp
)
target_include_directories(altmacro_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(altmacro_core PUBLIC
    Threads::Threads
    OpenSSL::SSL OpenSSL::Crypto
    spdlog::spdlog fmt::fmt
    nlohmann_json::nlohmann_json
    yaml-cpp::yaml-cpp
    Boost::system
    PkgConfig::PQXX
    PkgConfig::SYSTEMD
    PkgConfig::LZMA
    nats_static
)

add_executable(altmacro
    src/main.cpp
    src/altmacro/cli/run.cpp
    src/altmacro/cli/bootstrap_cmd.cpp
    src/altmacro/cli/inspect.cpp
    src/altmacro/cli/selftest.cpp
)
target_link_libraries(altmacro PRIVATE altmacro_core)

install(TARGETS altmacro RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Everything the target host needs and nothing it does not. No sources, no
# toolchain, no test fixtures: VPS1 has neither the -dev packages nor the disk.
install(DIRECTORY db/migrations/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/altmacro/migrations
        FILES_MATCHING PATTERN "*.sql")
install(FILES
            config/altmacro.example.yaml
            config/altmacro.env.example
        DESTINATION ${CMAKE_INSTALL_DATADIR}/altmacro/config-examples)
install(FILES
            deploy/systemd/altmacro.service
            deploy/systemd/altmacro-bootstrap.service
        DESTINATION lib/systemd/system)
install(PROGRAMS
            scripts/provision.sh
            scripts/db-bootstrap.sh
            scripts/nats-user.sh
            scripts/grant-downstream-fdw.sh
            scripts/cleanup-host.sh
        DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# The verification scripts travel with the release. They need a live database,
# a live broker and a live provider, so they cannot run in the unit-test suite
# -- and copying them by hand before every audit is how an audit quietly starts
# testing a different version from the one deployed.
install(PROGRAMS
            tests/shell/forensic_audit.sh
            tests/shell/test_schema_invariants.sh
            tests/shell/test_data_quality.sh
            tests/shell/test_restart_resume.sh
            tests/shell/test_provider_failure.sh
            tests/shell/test_db_failure.sh
            tests/shell/test_live_service.sh
            tests/shell/verify_jetstream.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/altmacro/verify)

if(ALTMACRO_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
