cmake_minimum_required(VERSION 3.28)

# altspotter — a self-contained multi-venue futures market-data platform.
#
# One binary, one service. `altspotter run` ingests, stores, backfills and
# quality-checks USDT-perpetual market data (candles, order books, raw trades,
# derivatives context) from four venues and publishes it over NATS + PostgreSQL
# for any consumer. The remaining subcommands (migrate, nats-init, config-check,
# inspect, retention-run, version) are the operational tools folded into the
# same binary.
#
# The consumer contract (Protobuf in proto/, schema in db/migrations/, the
# NATS subjects/streams and the PostgreSQL tables) is documented in connect.md.
project(altspotter VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(ALTSPOTTER_BUILD_TESTS "Build unit and integration tests" OFF)
option(ALTSPOTTER_ENABLE_SANITIZERS "Enable ASan/UBSan (debug builds)" OFF)

if(ALTSPOTTER_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# --- system dependencies (Ubuntu 24.04; lean data-plane deps only) ---
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)          # 3.0.13
find_package(Boost 1.83 REQUIRED)       # Asio/Beast header-only
find_package(Protobuf REQUIRED)         # 3.21.12
find_package(spdlog REQUIRED)           # 1.12.0
find_package(fmt REQUIRED)              # 9.1.0
find_package(nlohmann_json 3.11 REQUIRED)
find_package(yaml-cpp REQUIRED)         # 0.8.0
find_package(ZLIB REQUIRED)             # crc32 for OKX order-book checksums
find_package(PkgConfig REQUIRED)
find_package(PostgreSQL REQUIRED)       # libpq 16.x
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)   # 7.8.1
target_link_libraries(PkgConfig::PQXX INTERFACE PostgreSQL::PostgreSQL)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

# --- NATS C client: pinned release, built statically (not packaged by Ubuntu) ---
include(FetchContent)
set(NATS_BUILD_LIB_SHARED OFF CACHE INTERNAL "")
set(NATS_BUILD_LIB_STATIC ON  CACHE INTERNAL "")
set(NATS_BUILD_EXAMPLES  OFF CACHE INTERNAL "")
set(NATS_BUILD_STREAMING OFF CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
FetchContent_Declare(
    natsc
    URL https://github.com/nats-io/nats.c/archive/refs/tags/v3.13.0.tar.gz
    URL_HASH SHA256=f6ec9ee2ab367594b56dd3265e3561074ade7c3d7410a6f45a77704c5e537024
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(natsc)

# Strict warnings apply to our sources only (FetchContent deps configured above).
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    add_compile_options(-Wno-maybe-uninitialized)
endif()

add_subdirectory(proto)
add_subdirectory(src)

if(ALTSPOTTER_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 REQUIRED)
    add_subdirectory(tests)
endif()
