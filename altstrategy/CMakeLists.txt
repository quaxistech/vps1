cmake_minimum_required(VERSION 3.28)

# altstrategy — the strategy/signal decision layer, one independent service.
#
# A single binary (altstrategy-service, src/) runs five pipeline stages on their
# own threads inside one process, sharing one PostgreSQL pool and one NATS
# connection: it evaluates the compile-time strategy catalog over live market
# context (features/structure/regime from Alttech; predictions from AltML),
# ranks candidates, turns the winner into a concrete trade plan and a versioned
# TradeSignal, and shadow-scores every approved signal against real 1m candles.
#
# It produces DECISIONS, not orders: any consuming project (the downstream
# risk/execution layer, or anyone else) picks trade_signals/trade_plans up from
# the shared PostgreSQL and the ALTSTRATEGY_ANALYTICS NATS stream.
#
# Wire/storage contract: altstrategy.v1 protobuf (proto/), the shared PostgreSQL schema
# (db/migrations/) and the ALTML_ANALYTICS prediction stream it consumes.
project(altstrategy VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(ALTSTRATEGY_BUILD_TESTS "Build unit tests" OFF)
option(ALTSTRATEGY_ENABLE_SANITIZERS "Enable ASan/UBSan (debug builds)" OFF)

if(ALTSTRATEGY_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE ALTSTRATEGY_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT ALTSTRATEGY_GIT_COMMIT)
    set(ALTSTRATEGY_GIT_COMMIT "unknown")
endif()

# --- system dependencies (Ubuntu 24.04; lean decision-layer deps only:
#     no TA-Lib, no LightGBM, no OpenMP — indicators live in alttech, ML in altml) ---
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)          # 3.0.13
find_package(Boost 1.83 REQUIRED)       # Asio header-only
find_package(Protobuf REQUIRED)         # 3.21.12
find_package(spdlog REQUIRED)           # 1.12.0
find_package(fmt REQUIRED)              # 9.1.0
find_package(nlohmann_json 3.11 REQUIRED)
find_package(yaml-cpp REQUIRED)         # 0.8.0
find_package(PkgConfig REQUIRED)
find_package(PostgreSQL REQUIRED)       # libpq 16.x
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)   # 7.8.1
target_link_libraries(PkgConfig::PQXX INTERFACE PostgreSQL::PostgreSQL)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

# --- NATS C client: pinned release, built statically ---
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

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    add_compile_options(-Wno-maybe-uninitialized)
endif()

# Wire contract (generated protobuf), then the consolidated service under src/,
# then the operational tools and (optionally) the unit tests.
add_subdirectory(proto)
add_subdirectory(src)

add_subdirectory(tools/db-migrate)
add_subdirectory(tools/nats-init)
add_subdirectory(tools/config-check)
add_subdirectory(tools/version)
add_subdirectory(tools/replay)

if(ALTSTRATEGY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
