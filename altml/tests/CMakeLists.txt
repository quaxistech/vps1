include(Catch)

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
