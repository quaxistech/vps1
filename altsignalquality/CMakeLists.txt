cmake_minimum_required(VERSION 3.28)

# altsignalquality — the learned signal filter between AltStrategy and VPS2.
#
# One binary (altsignalquality), one process, one systemd unit. It consumes
# AltStrategy's candidate TradeSignalProposals, scores each one with a CPU
# LightGBM model trained in-process on this stack's own signal history, and
# publishes the FINAL TradeSignal for the ones it accepts.
#
# Dependency policy is the stack's: system packages for everything Ubuntu 24.04
# ships, and exactly two pinned FetchContent dependencies (nats.c, LightGBM) at
# the same revisions AltML uses, so the two ML services share one runtime and
# one model format. No Python toolchain, no GPU, no external AI service.
project(altsignalquality VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(ALTSIGNALQUALITY_BUILD_TESTS "Build unit and contract tests" ON)
option(ALTSIGNALQUALITY_ENABLE_SANITIZERS "Enable ASan/UBSan" OFF)

if(ALTSIGNALQUALITY_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
find_package(OpenMP REQUIRED)          # LightGBM
find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(yaml-cpp REQUIRED)
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

# Same pinned revision as AltML: one LightGBM, one model text format, one set of
# numerical semantics across the two services that produce models on this stack.
set(BUILD_STATIC_LIB ON CACHE INTERNAL "")
set(BUILD_CLI OFF CACHE INTERNAL "")
FetchContent_Declare(
    lightgbm
    GIT_REPOSITORY https://github.com/microsoft/LightGBM.git
    GIT_TAG d02a01ac6f51d36c9e62388243bcb75c3b1b1774
    EXCLUDE_FROM_ALL
    GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(lightgbm)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    add_compile_options(-Wno-maybe-uninitialized)
endif()

add_subdirectory(proto)

# --- the pure decision core -------------------------------------------------
# domain/ holds the feature contract, the label spec, the as-of priors, the
# temporal splits, the metrics, the calibration, the decision policy and the
# leakage audit. It links NOTHING but hashing and time, which is what makes it
# exhaustively unit-testable without a database, a broker or a model file.
add_library(altsignalquality_domain STATIC
    src/altsignalquality/core/hash.cpp
    src/altsignalquality/core/time.cpp
    src/altsignalquality/domain/feature_contract.cpp)
target_include_directories(altsignalquality_domain PUBLIC src)
target_link_libraries(altsignalquality_domain
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE OpenSSL::Crypto)

# --- shared runtime: config, logging, lifecycle, PostgreSQL, NATS, codec ----
add_library(altsignalquality_core STATIC
    src/altsignalquality/core/codec.cpp
    src/altsignalquality/core/config.cpp
    src/altsignalquality/core/logger.cpp
    src/altsignalquality/core/nats_client.cpp
    src/altsignalquality/core/nats_provision.cpp
    src/altsignalquality/core/nats_subjects.cpp
    src/altsignalquality/core/pg.cpp
    src/altsignalquality/core/readiness.cpp
    src/altsignalquality/core/shutdown.cpp
    src/altsignalquality/data/store.cpp)
target_include_directories(altsignalquality_core PUBLIC src)
target_link_libraries(altsignalquality_core
    PUBLIC altsignalquality_domain Threads::Threads yaml-cpp::yaml-cpp
           nlohmann_json::nlohmann_json altsignalquality_proto PkgConfig::PQXX nats_static
    PRIVATE OpenSSL::Crypto spdlog::spdlog fmt::fmt PkgConfig::SYSTEMD)

# --- the service binary -----------------------------------------------------
add_executable(altsignalquality
    src/main.cpp
    src/altsignalquality/cli/ablation.cpp
    src/altsignalquality/cli/dataset_build.cpp
    src/altsignalquality/cli/model_status.cpp
    src/altsignalquality/cli/nats_setup.cpp
    src/altsignalquality/cli/run.cpp
    src/altsignalquality/cli/train.cpp
    src/altsignalquality/cli/validate.cpp
    src/altsignalquality/components/dataset.cpp
    src/altsignalquality/components/evaluator.cpp
    src/altsignalquality/components/model_registry.cpp
    src/altsignalquality/components/model_runtime.cpp
    src/altsignalquality/components/supervisor.cpp
    src/altsignalquality/components/trainer.cpp)
target_compile_definitions(altsignalquality
    PRIVATE ALTSIGNALQUALITY_VERSION="${PROJECT_VERSION}")
target_include_directories(altsignalquality PRIVATE "${lightgbm_SOURCE_DIR}/include")
target_link_libraries(altsignalquality PRIVATE
    altsignalquality_core _lightgbm OpenMP::OpenMP_CXX)

if(ALTSIGNALQUALITY_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 REQUIRED)
    add_subdirectory(tests)
endif()

install(TARGETS altsignalquality RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(PROGRAMS
    scripts/provision.sh
    scripts/db-bootstrap.sh
    scripts/apply-additive-migrations.sh
    scripts/install-systemd-units.sh
    DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY db/migrations/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altsignalquality/db/migrations"
    FILES_MATCHING PATTERN "*.sql")
install(DIRECTORY proto/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altsignalquality/proto"
    FILES_MATCHING PATTERN "*.proto")
install(FILES config/altsignalquality.example.yaml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altsignalquality/config-examples")
install(FILES config/altsignalquality.env.example
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altsignalquality/config-examples"
    RENAME common.env.example)
install(DIRECTORY deploy/systemd/
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system"
    FILES_MATCHING PATTERN "*.service" PATTERN "*.conf")
install(FILES README.md docs/ARCHITECTURE.md docs/INTEGRATION.md docs/OPERATIONS.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
