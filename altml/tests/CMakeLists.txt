include(Catch)

add_executable(altml_artifact_tests unit/test_verified_artifact.cpp ../src/altml/core/hash.cpp)
target_include_directories(altml_artifact_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(altml_artifact_tests PRIVATE Catch2::Catch2WithMain OpenSSL::Crypto)
catch_discover_tests(altml_artifact_tests PROPERTIES LABELS unit)

add_executable(altml_unit_tests
    unit/test_config_defaults.cpp
    unit/test_econ_logic.cpp
    unit/test_feature_recipe.cpp
    unit/test_inference_logic.cpp
    unit/test_label_quality.cpp
    unit/test_retention_logic.cpp
    unit/test_trainer_logic.cpp
    unit/test_wire_contract.cpp)
target_link_libraries(altml_unit_tests PRIVATE
    Catch2::Catch2WithMain
    altml_proto
    altml_proto_alttech)
target_include_directories(altml_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
catch_discover_tests(altml_unit_tests PROPERTIES LABELS unit)

add_test(
    NAME altml_release_layout_contract
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/shell/test_ml_release_contract.sh"
            "${CMAKE_SOURCE_DIR}")
set_tests_properties(altml_release_layout_contract PROPERTIES LABELS unit)

# CLI smoke tests: every subcommand answers --help with exit 0.
foreach(args IN ITEMS "--help" "--version" "run;--help" "train;--help"
        "nats-setup;--help" "model-validate;--help" "replay;--help")
    string(REPLACE ";" "_" test_suffix "${args}")
    string(REPLACE "--" "" test_suffix "${test_suffix}")
    add_test(NAME "altml_cli_${test_suffix}" COMMAND $<TARGET_FILE:altml> ${args})
    set_tests_properties("altml_cli_${test_suffix}" PROPERTIES LABELS unit)
endforeach()

add_executable(altml_archive_tests unit/test_feature_archive.cpp)
target_include_directories(altml_archive_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(altml_archive_tests PRIVATE ALTML_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(altml_archive_tests PRIVATE altml_core altml_proto_alttech Catch2::Catch2WithMain)
catch_discover_tests(altml_archive_tests PROPERTIES LABELS integration)

add_executable(altml_model_buffer_tests unit/test_model_buffer.cpp)
target_include_directories(altml_model_buffer_tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${lightgbm_SOURCE_DIR}/include")
target_link_libraries(altml_model_buffer_tests PRIVATE altml_core _lightgbm OpenMP::OpenMP_CXX Catch2::Catch2WithMain)
catch_discover_tests(altml_model_buffer_tests PROPERTIES LABELS unit)

# Exercise the same inference translation unit and ACK/NAK loop as the daemon.
# The compiler cache can reuse its object; no production-only test hooks.
add_executable(altml_archive_consumer_probe integration/archive_consumer_probe.cpp
    ../src/altml/components/inference.cpp)
target_compile_definitions(altml_archive_consumer_probe PRIVATE
    ALTML_VERSION="${PROJECT_VERSION}")
set_source_files_properties(integration/archive_consumer_probe.cpp PROPERTIES
    COMPILE_DEFINITIONS ALTML_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(altml_archive_consumer_probe PRIVATE "${lightgbm_SOURCE_DIR}/include")
target_link_libraries(altml_archive_consumer_probe PRIVATE
    altml_core altml_proto_alttech _lightgbm OpenMP::OpenMP_CXX)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_executable(altml_window_migration_tests integration/input_window_migration.cpp)
    target_link_libraries(altml_window_migration_tests PRIVATE altml_core)
    add_test(NAME altml_input_window_migration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/input_window_migration.py
                $<TARGET_FILE:altml_window_migration_tests>)
    set_tests_properties(altml_input_window_migration PROPERTIES LABELS integration TIMEOUT 45)
    add_test(NAME altml_archive_consumer_recovery
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/archive_consumer.py
                $<TARGET_FILE:altml_archive_consumer_probe>)
    set_tests_properties(altml_archive_consumer_recovery PROPERTIES
        LABELS integration SKIP_RETURN_CODE 77 TIMEOUT 180)
    add_test(NAME altml_model_loader_refusals
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/model_loader_refusals.py
                $<TARGET_FILE:altml_archive_consumer_probe>)
    set_tests_properties(altml_model_loader_refusals PROPERTIES
        LABELS integration SKIP_RETURN_CODE 77 TIMEOUT 90)
endif()
