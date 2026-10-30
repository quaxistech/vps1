cmake_minimum_required(VERSION 3.28)

project(alttech VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(ALTTECH_BUILD_TESTS "Build AltTech unit and integration tests" ON)
option(ALTTECH_ENABLE_SANITIZERS "Enable ASan/UBSan in AltTech targets" OFF)
# Bare-metal tuning. Both default OFF so the portable release stays reproducible;
# enable them in the build that runs ON the deployment host (the produced binary
# is then specific to that CPU and must not be copied to a different micro-arch).
option(ALTTECH_NATIVE_ARCH "Tune generated code for the building host CPU (-march=native)" OFF)
# Carry the C++ runtime inside the binaries instead of linking it dynamically.
# Only needed when the build host's toolchain is newer than the target's: the
# binary then depends on glibc alone. Off by default so libstdc++ security
# updates still arrive through the distribution.
option(ALTTECH_PORTABLE_RUNTIME "Link libstdc++ and libgcc statically" OFF)
option(ALTTECH_ENABLE_LTO "Enable link-time optimization (IPO) for all AltTech targets" OFF)

if(ALTTECH_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(ALTTECH_NATIVE_ARCH)
    add_compile_options(-march=native -mtune=native)
endif()

if(ALTTECH_PORTABLE_RUNTIME)
    add_link_options(-static-libstdc++ -static-libgcc)
endif()

if(ALTTECH_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ALTTECH_IPO_OK OUTPUT ALTTECH_IPO_MSG)
    if(ALTTECH_IPO_OK)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        message(STATUS "AltTech: link-time optimization enabled")
    else()
        message(WARNING "AltTech: LTO requested but unsupported: ${ALTTECH_IPO_MSG}")
    endif()
endif()

set(ALTTECH_BUILD_REVISION "" CACHE STRING
    "Release or source revision reported by alttech-version")
if(NOT ALTTECH_BUILD_REVISION)
    execute_process(
        COMMAND git rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE ALTTECH_BUILD_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT ALTTECH_BUILD_REVISION)
    set(ALTTECH_BUILD_REVISION "source-tree")
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(PostgreSQL REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

include(FetchContent)

# Source dependencies are pinned by SHA-256 and statically linked into the
# binaries, so a build is reproducible and independent of what a host distro
# happens to package.
#
# libpqxx is built here rather than taken from the distribution on purpose:
# distributions compile it as C++17, but its headers add a std::source_location
# parameter to several constructors under C++20 and later. Linking a C++23
# program against a C++17 build of the library therefore fails — and whether it
# fails depends on which standard headers a translation unit happened to include
# first, so it can succeed on one compiler version and fail on the next.
# Building it with our own standard removes that class of failure entirely.
set(NATS_BUILD_LIB_SHARED OFF CACHE INTERNAL "")
set(NATS_BUILD_LIB_STATIC ON CACHE INTERNAL "")
set(NATS_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(NATS_BUILD_STREAMING OFF CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
FetchContent_Declare(
    natsc
    URL https://github.com/nats-io/nats.c/archive/refs/tags/v3.13.0.tar.gz
    URL_HASH SHA256=f6ec9ee2ab367594b56dd3265e3561074ade7c3d7410a6f45a77704c5e537024
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(natsc)

set(SKIP_BUILD_TEST ON CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
set(BUILD_DOC OFF CACHE INTERNAL "")
FetchContent_Declare(
    libpqxx
    URL https://github.com/jtv/libpqxx/archive/refs/tags/7.9.2.tar.gz
    URL_HASH SHA256=e37d5774c39f6c802e32d7f418e88b8e530404fb54758516e884fc0ebdee6da4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(libpqxx)
# The generated headers are noisy under our warning set; they are not our code.
target_compile_options(pqxx PRIVATE -w)
add_library(alttech_pqxx INTERFACE)
target_link_libraries(alttech_pqxx INTERFACE pqxx PostgreSQL::PostgreSQL)

FetchContent_Declare(
    talib
    URL https://github.com/TA-Lib/ta-lib/releases/download/v0.7.1/ta-lib-0.7.1-src.tar.gz
    URL_HASH SHA256=508981a5b85edab42ecee0b2d9c7dcd2c4ae9831e859e1aa4e549232734c27e1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(talib)
file(GLOB TALIB_SOURCES
    "${talib_SOURCE_DIR}/src/ta_func/*.c"
    "${talib_SOURCE_DIR}/src/ta_common/*.c"
    "${talib_SOURCE_DIR}/src/ta_abstract/*.c"
    "${talib_SOURCE_DIR}/src/ta_abstract/frames/*.c"
    "${talib_SOURCE_DIR}/src/ta_abstract/tables/*.c")
add_library(talib STATIC ${TALIB_SOURCES})
target_include_directories(talib
    PUBLIC "${talib_SOURCE_DIR}/include"
    PRIVATE
        "${talib_SOURCE_DIR}/src/ta_common"
        "${talib_SOURCE_DIR}/src/ta_func"
        "${talib_SOURCE_DIR}/src/ta_abstract"
        "${talib_SOURCE_DIR}/src/ta_abstract/frames")
target_compile_options(talib PRIVATE -w)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    add_compile_options(-Wno-maybe-uninitialized)
endif()

add_subdirectory(proto)

set(ALTTECH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/include")

add_library(alttech_common STATIC
    src/common/hash.cpp
    src/common/readiness.cpp
    src/common/shutdown.cpp)
target_include_directories(alttech_common PUBLIC "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech_common
    PUBLIC Threads::Threads
    PRIVATE OpenSSL::Crypto PkgConfig::SYSTEMD)

add_library(alttech_time STATIC src/time/time.cpp)
target_include_directories(alttech_time PUBLIC "${ALTTECH_INCLUDE_DIR}")

add_library(alttech_logging STATIC src/logging/logger.cpp)
target_include_directories(alttech_logging PUBLIC "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech_logging
    PUBLIC nlohmann_json::nlohmann_json alttech_time
    PRIVATE spdlog::spdlog fmt::fmt PkgConfig::SYSTEMD)

add_library(alttech_domain STATIC
    src/domain/decimal.cpp)
target_include_directories(alttech_domain PUBLIC "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech_domain PUBLIC alttech_common)

add_library(alttech_config STATIC
    src/config/config.cpp
    src/config/service_bootstrap.cpp
    src/config/analytics_config.cpp)
target_include_directories(alttech_config PUBLIC "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech_config
    PUBLIC alttech_common yaml-cpp::yaml-cpp
    PRIVATE alttech_logging)

add_library(alttech_messaging STATIC
    src/messaging/codec.cpp
    src/messaging/nats_client.cpp)
target_include_directories(alttech_messaging PUBLIC "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech_messaging
    PUBLIC alttech_common alttech_domain alttech_time alttech_proto nats_static
    PRIVATE alttech_logging)

add_library(alttech_persistence STATIC src/persistence/pg.cpp)
target_include_directories(alttech_persistence PUBLIC "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech_persistence
    PUBLIC alttech_common alttech_domain alttech_time alttech_logging alttech_pqxx)

add_library(alttech_application STATIC src/application/module_supervisor.cpp)
target_include_directories(alttech_application PUBLIC "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech_application PUBLIC alttech_common PRIVATE alttech_logging alttech_time)

add_library(alttech_talib_features STATIC
    src/talib_features/engine.cpp
    src/talib_features/feature_engine.cpp)
target_include_directories(alttech_talib_features PUBLIC "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech_talib_features PUBLIC alttech_common PRIVATE talib)

add_library(alttech_feature_module STATIC
    src/modules/feature/feature_module.cpp
    src/modules/feature/feature_store.cpp)
target_include_directories(alttech_feature_module
    PUBLIC "${ALTTECH_INCLUDE_DIR}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/feature")
target_link_libraries(alttech_feature_module PRIVATE
    alttech_common alttech_config alttech_logging alttech_time alttech_domain
    alttech_messaging alttech_persistence alttech_talib_features
    alttech_venue_lite)

add_library(alttech_microstructure_module STATIC
    src/modules/microstructure/microstructure_module.cpp)
target_include_directories(alttech_microstructure_module PUBLIC "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech_microstructure_module PRIVATE
    alttech_common alttech_config alttech_logging alttech_time alttech_domain
    alttech_messaging alttech_persistence
    alttech_venue_lite)

add_library(alttech_structure_module STATIC
    src/modules/structure/structure_module.cpp)
target_include_directories(alttech_structure_module PUBLIC "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech_structure_module PRIVATE
    alttech_common alttech_config alttech_logging alttech_time alttech_domain
    alttech_messaging alttech_persistence
    alttech_venue_lite)

add_library(alttech_regime_module STATIC
    src/modules/regime/regime_module.cpp)
target_include_directories(alttech_regime_module PUBLIC "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech_regime_module PRIVATE
    alttech_common alttech_config alttech_logging alttech_time alttech_domain
    alttech_messaging alttech_persistence alttech_talib_features
    alttech_venue_lite)

# Pure macro / cross-asset computation: returns, realized volatility, the
# spreads between the three CNY observations, rate-curve constructions. No
# PostgreSQL and no NATS, so the arithmetic and the as-of rules are testable
# without a database -- which matters here because a look-ahead bug would be
# invisible in production and fatal in a model.
add_library(alttech_macro_core STATIC
    src/modules/macro/macro_compute.cpp
    src/modules/macro/macro_availability.cpp
    src/modules/macro/macro_features.cpp)
target_include_directories(alttech_macro_core
    PUBLIC "${ALTTECH_INCLUDE_DIR}"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/macro")
target_link_libraries(alttech_macro_core PRIVATE OpenSSL::Crypto)

add_library(alttech_macro_module STATIC
    src/modules/macro/macro_module.cpp)
target_include_directories(alttech_macro_module
    PUBLIC "${ALTTECH_INCLUDE_DIR}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/macro")
target_link_libraries(alttech_macro_module PRIVATE
    alttech_common alttech_config alttech_logging alttech_time alttech_domain
    alttech_messaging alttech_persistence alttech_macro_core)

# Pure, dependency-light cross-venue analytics engine (consensus, quality,
# alignment, normalization, lead-lag). No PostgreSQL/NATS/protobuf: unit-testable
# in isolation and reused by the cross_venue module runner.
add_library(alttech_cross_venue_core STATIC
    src/modules/cross_venue/consensus.cpp
    src/modules/cross_venue/normalization.cpp
    src/modules/cross_venue/alignment.cpp
    src/modules/cross_venue/lead_lag.cpp
    src/modules/cross_venue/global_view.cpp)
target_include_directories(alttech_cross_venue_core
    PUBLIC "${ALTTECH_INCLUDE_DIR}"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/cross_venue")

# Per-venue reference_lite engine (pure) + its store/sweep. The four legacy
# modules link this to extend from Bitget-only to all four venues.
add_library(alttech_venue_lite_core STATIC
    src/modules/venue_lite/reference_engine.cpp)
target_include_directories(alttech_venue_lite_core
    PUBLIC "${ALTTECH_INCLUDE_DIR}"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/venue_lite")

add_library(alttech_venue_lite STATIC
    src/modules/venue_lite/venue_store.cpp
    src/modules/venue_lite/venue_sweep.cpp)
target_include_directories(alttech_venue_lite
    PUBLIC "${ALTTECH_INCLUDE_DIR}"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/venue_lite")
target_link_libraries(alttech_venue_lite PUBLIC
    alttech_venue_lite_core
    PRIVATE alttech_common alttech_config alttech_logging alttech_time alttech_domain
            alttech_messaging alttech_persistence)

add_library(alttech_cross_venue_module STATIC
    src/modules/cross_venue/cross_venue_module.cpp
    src/modules/cross_venue/cross_venue_store.cpp)
target_include_directories(alttech_cross_venue_module
    PUBLIC "${ALTTECH_INCLUDE_DIR}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/cross_venue")
target_link_libraries(alttech_cross_venue_module PRIVATE
    alttech_common alttech_config alttech_logging alttech_time alttech_domain
    alttech_messaging alttech_persistence alttech_cross_venue_core)

add_executable(alttech-service src/app/main.cpp)
target_include_directories(alttech-service PRIVATE "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech-service PRIVATE
    alttech_feature_module
    alttech_microstructure_module
    alttech_structure_module
    alttech_regime_module
    alttech_cross_venue_module
    alttech_macro_module
    alttech_application
    alttech_config alttech_messaging alttech_persistence)

add_executable(alttech-schema src/tools/schema/main.cpp)
target_include_directories(alttech-schema PRIVATE "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech-schema
    PRIVATE alttech_common alttech_logging alttech_pqxx)

add_executable(alttech-nats-setup src/tools/nats-setup/main.cpp)
target_include_directories(alttech-nats-setup PRIVATE "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech-nats-setup PRIVATE alttech_logging nats_static)

add_executable(alttech-config-check src/tools/config-check/main.cpp)
target_include_directories(alttech-config-check PRIVATE "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech-config-check PRIVATE alttech_config)

add_executable(alttech-analytics src/tools/analytics/main.cpp)
target_include_directories(alttech-analytics PRIVATE "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech-analytics
    PRIVATE alttech_common alttech_config alttech_logging alttech_pqxx)

# Independent TA-Lib reference check: calls the concrete TA_* functions rather
# than the abstract interface the service uses, so the two cannot agree by
# sharing a fault. Ships with the release for on-host verification.
add_executable(alttech-talib-verify src/tools/talib-verify/main.cpp)
target_link_libraries(alttech-talib-verify PRIVATE talib)

# Historical reconstruction of talib-core vectors from raw 1m candles (stdin
# CSV -> stdout CSV), with the live aggregation and the backfill's engine pass.
# Touches no database: a dataset built with it is a function of its snapshot.
add_executable(alttech-dataset src/tools/dataset/main.cpp)
target_include_directories(alttech-dataset PRIVATE "${ALTTECH_INCLUDE_DIR}")
target_link_libraries(alttech-dataset PRIVATE alttech_talib_features)

add_executable(alttech-version src/tools/version/main.cpp)
target_compile_definitions(alttech-version PRIVATE
    ALTTECH_VERSION="${PROJECT_VERSION}"
    ALTTECH_REVISION="${ALTTECH_BUILD_REVISION}")

if(ALTTECH_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 REQUIRED)
    add_subdirectory(tests)
endif()

install(TARGETS
    alttech-service
    alttech-schema
    alttech-nats-setup
    alttech-config-check
    alttech-analytics
    alttech-version
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES db/schema.sql
    DESTINATION "${CMAKE_INSTALL_DATADIR}/alttech")
install(DIRECTORY db/upgrades/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/alttech/upgrades"
    FILES_MATCHING PATTERN "*.sql")
install(DIRECTORY proto/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/alttech/proto"
    FILES_MATCHING PATTERN "*.proto")
install(FILES "${CMAKE_BINARY_DIR}/alttech_schema.desc"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/alttech/proto")
install(DIRECTORY config/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/alttech/config-examples"
    FILES_MATCHING PATTERN "*.example.yaml")
install(FILES .env.example
    DESTINATION "${CMAKE_INSTALL_DATADIR}/alttech/config-examples"
    RENAME common.env.example)
install(DIRECTORY deploy/systemd/
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system"
    FILES_MATCHING PATTERN "*.service" PATTERN "*.target" PATTERN "*.timer")
install(FILES README.md connect.md docs/ARCHITECTURE.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
