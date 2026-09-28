# All modules compile into one static library; the `altspotter` executable is a
# thin main() over it. The library is also what the test target links against.
# Headers are included module-relative (e.g. "messaging/codec.hpp") from src/.

add_library(altspotter_lib STATIC
    core/hash.cpp
    core/readiness.cpp
    core/shutdown.cpp
    core/time.cpp
    core/decimal.cpp
    core/types.cpp

    logging/logger.cpp
    config/config.cpp

    messaging/codec.cpp
    messaging/nats_client.cpp

    persistence/pg.cpp
    persistence/candle_store.cpp
    persistence/retention.cpp

    exchanges/rest_client.cpp
    exchanges/order_book.cpp
    exchanges/venue_ws.cpp
    exchanges/bitget/bitget_adapter.cpp
    exchanges/bitget/bitget_ws.cpp
    exchanges/bitget/bitget_context.cpp
    exchanges/binance/binance_adapter.cpp
    exchanges/bybit/bybit_adapter.cpp
    exchanges/okx/okx_adapter.cpp

    components/runner.cpp
    components/instrument_catalog.cpp
    components/market_gateway.cpp
    components/microstructure_collector.cpp
    components/market_history.cpp
    components/history_owner.cpp
    components/storage_writer.cpp
    components/bar_aggregation.cpp
    components/data_integrity.cpp
    components/exchange_runtime.cpp
    components/microstructure_aggregation.cpp
    components/retention_manager.cpp

    ops/migrate.cpp
    ops/nats_init.cpp
    ops/config_check.cpp
    ops/inspect.cpp
    ops/retention.cpp
    ops/version.cpp)

target_include_directories(altspotter_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(altspotter_lib PRIVATE
    ALTSPOTTER_VERSION="${PROJECT_VERSION}")

target_link_libraries(altspotter_lib PUBLIC
    altspotter_proto
    nats_static
    Threads::Threads
    Boost::boost
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
    yaml-cpp::yaml-cpp
    ZLIB::ZLIB
    PkgConfig::PQXX
    PkgConfig::SYSTEMD)

add_executable(altspotter main.cpp)
target_link_libraries(altspotter PRIVATE altspotter_lib)
