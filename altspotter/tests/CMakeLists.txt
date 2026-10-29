# Native integration test. SKIPs unless a nats-server binary and an admin
# PostgreSQL DSN ($ALTSPOTTER_TEST_NATS_SERVER / $ALTSPOTTER_TEST_PG_DSN) are
# available. Links the shared library so it exercises the exact production code.

add_executable(altspotter_tests
    test_pipeline_integration.cpp
    test_adapters.cpp
    test_order_book.cpp
    test_gap_repair.cpp
    test_agg_boundary.cpp
    test_agg_catchup_e2e.cpp
    test_funding_sync.cpp
    test_history_runs.cpp
    test_history_orphans_e2e.cpp
    test_depth_consistency.cpp
    test_env.cpp)
target_sources(altspotter_tests PRIVATE test_candle_corrections.cpp)

target_include_directories(altspotter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(altspotter_tests PRIVATE
    ALTSPOTTER_BIN_DIR="${CMAKE_BINARY_DIR}/bin"
    ALTSPOTTER_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations")

target_link_libraries(altspotter_tests PRIVATE
    altspotter_lib
    Catch2::Catch2WithMain)

# The test drives the installed `altspotter` binary, so make sure it is built.
add_dependencies(altspotter_tests altspotter)

add_test(NAME pipeline COMMAND altspotter_tests)
set_tests_properties(pipeline PROPERTIES SKIP_RETURN_CODE 4)
