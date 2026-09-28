cmake_minimum_required(VERSION 3.28)

project(altml VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(ALTML_BUILD_TESTS "Build AltML unit and contract tests" ON)
option(ALTML_ENABLE_SANITIZERS "Enable ASan/UBSan for AltML targets" OFF)

if(ALTML_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
find_package(OpenMP REQUIRED)
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
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    add_compile_options(-Wno-maybe-uninitialized)
endif()

add_subdirectory(proto)

# Shared runtime: config, logging, shutdown/readiness, time, hashing,
# PostgreSQL pool, NATS client/codec, JetStream provisioning.
add_library(altml_core STATIC
    src/altml/core/codec.cpp
    src/altml/core/config.cpp
    src/altml/core/hash.cpp
    src/altml/core/logger.cpp
    src/altml/core/nats_client.cpp
    src/altml/core/nats_provision.cpp
    src/altml/core/pg.cpp
    src/altml/core/readiness.cpp
    src/altml/core/shutdown.cpp
    src/altml/core/time.cpp)
target_include_directories(altml_core PUBLIC src)
target_link_libraries(altml_core
    PUBLIC Threads::Threads yaml-cpp::yaml-cpp nlohmann_json::nlohmann_json
           altml_proto nats_static PkgConfig::PQXX
    PRIVATE OpenSSL::Crypto spdlog::spdlog fmt::fmt PkgConfig::SYSTEMD)

# The unified service binary.
add_executable(altml
    src/main.cpp
    src/altml/cli/model_validate.cpp
    src/altml/cli/nats_setup.cpp
    src/altml/cli/run.cpp
    src/altml/cli/train.cpp
    src/altml/cli/replay.cpp
    src/altml/components/inference.cpp
    src/altml/components/label_builder.cpp
    src/altml/components/model_registry.cpp
    src/altml/components/storage_guard.cpp
    src/altml/components/supervisor.cpp
    src/altml/components/trainer.cpp)
target_compile_definitions(altml PRIVATE ALTML_VERSION="${PROJECT_VERSION}")
target_include_directories(altml PRIVATE "${lightgbm_SOURCE_DIR}/include")
target_link_libraries(altml PRIVATE
    altml_core altml_proto_alttech _lightgbm OpenMP::OpenMP_CXX)

if(ALTML_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 REQUIRED)
    add_subdirectory(tests)
endif()

install(TARGETS altml RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(PROGRAMS
    scripts/provision.sh
    scripts/install-systemd-units.sh
    scripts/db-bootstrap.sh
    DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES db/schema.sql
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altml/db")
install(DIRECTORY proto/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altml/proto"
    FILES_MATCHING PATTERN "*.proto")
install(FILES config/altml.example.yaml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altml/config-examples")
# /etc/altml/common.env is a deployed-host contract referenced by every unit;
# keep the installed example under its historical name.
install(FILES config/altml.env.example
    DESTINATION "${CMAKE_INSTALL_DATADIR}/altml/config-examples"
    RENAME common.env.example)
install(DIRECTORY deploy/systemd/
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system"
    FILES_MATCHING
        PATTERN "*.service"
        PATTERN "*.timer"
        PATTERN "*.conf")
install(FILES README.md docs/INTEGRATION.md docs/ARCHITECTURE.md docs/OPERATIONS.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
