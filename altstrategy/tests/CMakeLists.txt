# Unit + integration tests for the adaptive decision engine.
#
#   altstrategy_engine_tests  — pure decision logic over the engine library
#       (catalogue, ensemble, allocator, ranking, plans, proposals, outcomes,
#        attribution, drift, lifecycle) + the full-path integration test.
#   altstrategy_io_tests      — the IO helpers that are DB/NATS-free and testable
#       (context vector decode, snapshot columnar writer + manifest + checksums).
find_package(Catch2 3 REQUIRED)
include(Catch)

# WHOLE_ARCHIVE so the compile-time strategy self-registrations are not stripped.
add_executable(altstrategy_engine_tests
    unit/test_engine_strategies.cpp
    unit/test_engine_ranking.cpp
    unit/test_engine_plan_proposal.cpp
    unit/test_engine_plan_units.cpp
    unit/test_engine_ml_evaluation.cpp
    unit/test_engine_outcomes.cpp
    unit/test_engine_outcomes_v2.cpp
    unit/test_engine_cost_model.cpp
    unit/test_engine_economic_policy.cpp
    unit/test_engine_integration.cpp)
target_link_libraries(altstrategy_engine_tests PRIVATE altstrategy_engine Catch2::Catch2WithMain)
catch_discover_tests(altstrategy_engine_tests)

add_executable(altstrategy_io_tests
    unit/test_plan_config.cpp
    unit/test_context_json.cpp
    unit/test_io_assembler_exporter.cpp
    unit/test_wire_contract.cpp)
target_link_libraries(altstrategy_io_tests PRIVATE altstrategy_engine_io altstrategy_proto Catch2::Catch2WithMain)
catch_discover_tests(altstrategy_io_tests)

# Against a real PostgreSQL 16 and a real JetStream, both created for the run in
# a temporary directory (PGBIN = the server binaries, nats-server on PATH);
# without them the cases SKIP. Publication after archive (BUG-01/02).
add_executable(altstrategy_integration_tests integration/test_publication_outbox.cpp)
target_include_directories(altstrategy_integration_tests PRIVATE unit)
target_compile_definitions(altstrategy_integration_tests PRIVATE
    ALTSTRATEGY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(altstrategy_integration_tests PRIVATE
    altstrategy_engine_io altstrategy_engine Catch2::Catch2WithMain)
catch_discover_tests(altstrategy_integration_tests PROPERTIES LABELS integration SKIP_RETURN_CODE 4)

# Hand-assembled parameterised SQL is validated only by the database at runtime.
# A 67-column INSERT once shipped with 68 placeholders and the whole suite still
# passed, because nothing here executes real SQL -- it surfaced in production as
# proposals that silently stopped being archived.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME sql_insert_arity
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_sql_arity.py)
    set_tests_properties(sql_insert_arity PROPERTIES LABELS static)
endif()
