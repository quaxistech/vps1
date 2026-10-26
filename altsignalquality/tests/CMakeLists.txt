# Unit + contract tests.
#
#   altsignalquality_domain_tests — the pure decision core: the feature
#       contract and its assembler, the label spec, the as-of priors, the
#       temporal splits, the filter metrics, the calibration, the decision
#       policy and the leakage audit. No database, no broker, no model file.
#   altsignalquality_io_tests     — the pieces that touch a contract boundary
#       without needing a live service: the PostgreSQL value codecs, the
#       deterministic id derivation, and the frozen wire schema.
include(Catch)

add_executable(altsignalquality_domain_tests
    unit/test_feature_contract.cpp
    unit/test_labeling.cpp
    unit/test_priors.cpp
    unit/test_split.cpp
    unit/test_calibration.cpp
    unit/test_metrics.cpp
    unit/test_policy.cpp
    unit/test_leakage.cpp
    unit/test_regressions.cpp)
target_link_libraries(altsignalquality_domain_tests PRIVATE
    altsignalquality_domain Catch2::Catch2WithMain)
catch_discover_tests(altsignalquality_domain_tests PROPERTIES LABELS unit)

add_executable(altsignalquality_io_tests
    unit/test_publication_evidence.cpp
    unit/test_pg_codec.cpp
    unit/test_identifiers.cpp
    unit/test_wire_contract.cpp)
target_link_libraries(altsignalquality_io_tests PRIVATE
    altsignalquality_core altsignalquality_proto Catch2::Catch2WithMain)
catch_discover_tests(altsignalquality_io_tests PROPERTIES LABELS unit)

# The vendored altstrategy.v1 contract must stay byte-identical to the producer
# when the sibling checkout is present. Skipped (not failed) when it is not.
add_test(NAME altsignalquality_vendored_proto_matches_producer
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/shell/test_vendored_proto.sh"
                 "${CMAKE_SOURCE_DIR}")
set_tests_properties(altsignalquality_vendored_proto_matches_producer
                     PROPERTIES LABELS contract SKIP_RETURN_CODE 77)

# Release-layout contract: the binary answers every documented subcommand, the
# schema migration and the config example ship, and no secret is committed.
add_test(NAME altsignalquality_release_contract
         COMMAND bash "${CMAKE_SOURCE_DIR}/tests/shell/test_release_contract.sh"
                 "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:altsignalquality>")
set_tests_properties(altsignalquality_release_contract PROPERTIES LABELS contract)

foreach(args IN ITEMS "--help" "--version" "run;--help" "train;--help" "validate;--help"
        "model-status;--help" "dataset-build;--help" "nats-setup;--help")
    string(REPLACE ";" "_" test_suffix "${args}")
    string(REPLACE "--" "" test_suffix "${test_suffix}")
    add_test(NAME "altsignalquality_cli_${test_suffix}"
             COMMAND $<TARGET_FILE:altsignalquality> ${args})
    set_tests_properties("altsignalquality_cli_${test_suffix}" PROPERTIES LABELS unit)
endforeach()

# Integration tests. NOT registered with ctest: they publish a real message onto
# a real stream and need a running service, a broker and a database. Build them
# always (so they cannot rot), run them deliberately:
#
#   ALTSQ_IT_DSN=postgresql://... ALTSQ_IT_NATS=nats://... \
#     ./bin/altsignalquality_integration_tests
add_executable(altsignalquality_integration_tests integration/test_live_path.cpp)
target_link_libraries(altsignalquality_integration_tests PRIVATE
    altsignalquality_core altsignalquality_proto Catch2::Catch2WithMain)

add_executable(altsignalquality_publication_store_tests unit/test_publication_store.cpp)
target_compile_definitions(altsignalquality_publication_store_tests PRIVATE ALTSQ_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(altsignalquality_publication_store_tests PRIVATE altsignalquality_core Catch2::Catch2WithMain)
catch_discover_tests(altsignalquality_publication_store_tests PROPERTIES LABELS integration)
