# Unit and contract tests.
#
# Unit tests run without a database, a broker or a socket. The paths that
# genuinely need those — the whole ingest-to-WebSocket lifecycle, crash
# recovery, failure injection and the real-signal end-to-end proof — are driven
# by tests/shell against the deployed stack, because a mock of PostgreSQL's
# transactional guarantees would be testing the mock.

add_executable(altsignalgateway_tests
    unit/test_auth.cpp
    unit/test_canonical.cpp
    unit/test_cursor_watermark.cpp
    unit/test_frame_encoding.cpp
    unit/test_delivery_state.cpp
    unit/test_hash_ids.cpp
    unit/test_protocol.cpp
    unit/test_query.cpp
    unit/test_validation.cpp
    unit/test_wire_contract.cpp
    unit/test_config.cpp)
target_link_libraries(altsignalgateway_tests PRIVATE
    altsignalgateway_ws altsignalgateway_core Catch2::Catch2WithMain)
target_compile_definitions(altsignalgateway_tests PRIVATE
    ALTSIGNALGATEWAY_VERSION="${PROJECT_VERSION}"
    ALTSIGNALGATEWAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

include(Catch)
catch_discover_tests(altsignalgateway_tests)

# Native owner-read cutoff test against a dedicated local fixture database.
# It never runs a deployed-stack shell probe or contacts a production server.
add_executable(altsignalgateway_macro_store_tests integration/test_macro_availability.cpp)
target_link_libraries(altsignalgateway_macro_store_tests PRIVATE
    altsignalgateway_core Catch2::Catch2WithMain)
catch_discover_tests(altsignalgateway_macro_store_tests PROPERTIES LABELS integration)

# --- adversarial publisher ----------------------------------------------------
# Deliberately NOT installed by the release: it publishes to a subject this
# service is denied, so it exists only to be carried to a host by hand when a
# malformed-input test is being run.
add_executable(asgw-inject tools/inject.cpp)
target_link_libraries(asgw-inject PRIVATE altsignalgateway_proto nats_static)

# --- shell contract tests -----------------------------------------------------
add_test(NAME release_contract
         COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/shell/test_release_contract.sh"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME vendored_proto
         COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/shell/test_vendored_proto.sh"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_test(NAME schema_invariants
         COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/shell/test_schema_invariants.sh"
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
