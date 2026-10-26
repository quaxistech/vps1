include(Catch)

add_executable(alttech_unit_tests
    unit/test_feature_service_logic.cpp
    unit/test_shutdown.cpp)
target_link_libraries(alttech_unit_tests PRIVATE
    Catch2::Catch2WithMain
    alttech_talib_features
    alttech_persistence
    talib)
target_include_directories(alttech_unit_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
    "${CMAKE_SOURCE_DIR}/src/modules/feature")
catch_discover_tests(alttech_unit_tests PROPERTIES LABELS unit TIMEOUT 1800)

add_executable(alttech_zone_lifecycle_tests unit/test_zone_lifecycle.cpp)
target_link_libraries(alttech_zone_lifecycle_tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(alttech_zone_lifecycle_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
    "${CMAKE_SOURCE_DIR}/src/modules/structure")
catch_discover_tests(alttech_zone_lifecycle_tests PROPERTIES LABELS unit TIMEOUT 30)


add_executable(alttech_zone_merge_tests unit/test_zone_merge.cpp)
target_link_libraries(alttech_zone_merge_tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(alttech_zone_merge_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
    "${CMAKE_SOURCE_DIR}/src/modules/structure")
catch_discover_tests(alttech_zone_merge_tests PROPERTIES LABELS unit TIMEOUT 30)

add_executable(alttech_sql_digest_tests unit/test_sql_digest.cpp)
target_link_libraries(alttech_sql_digest_tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(alttech_sql_digest_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
    "${CMAKE_SOURCE_DIR}/src/tools/schema")
catch_discover_tests(alttech_sql_digest_tests PROPERTIES LABELS unit TIMEOUT 30)

add_executable(alttech_maintenance_lock_tests unit/test_maintenance_lock.cpp)
target_link_libraries(alttech_maintenance_lock_tests PRIVATE Catch2::Catch2WithMain alttech_pqxx)
target_include_directories(alttech_maintenance_lock_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include")
catch_discover_tests(alttech_maintenance_lock_tests PROPERTIES LABELS unit TIMEOUT 30)

add_executable(alttech_talib_coverage_tests unit/test_talib_coverage.cpp)
target_link_libraries(alttech_talib_coverage_tests PRIVATE
    Catch2::Catch2WithMain
    alttech_talib_features
    talib)
target_include_directories(alttech_talib_coverage_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include")
catch_discover_tests(alttech_talib_coverage_tests PROPERTIES LABELS unit TIMEOUT 300)

add_executable(alttech_supervisor_tests unit/test_module_supervisor.cpp)
target_link_libraries(alttech_supervisor_tests PRIVATE
    Catch2::Catch2WithMain
    alttech_application)
target_include_directories(alttech_supervisor_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include")
catch_discover_tests(alttech_supervisor_tests PROPERTIES LABELS unit TIMEOUT 30)

add_executable(alttech_wire_tests unit/test_wire_contract.cpp)
target_link_libraries(alttech_wire_tests PRIVATE
    Catch2::Catch2WithMain
    alttech_messaging)
target_include_directories(alttech_wire_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include")
catch_discover_tests(alttech_wire_tests PROPERTIES LABELS unit TIMEOUT 30)

add_executable(alttech_topology_tests unit/test_nats_topology.cpp)
target_link_libraries(alttech_topology_tests PRIVATE
    Catch2::Catch2WithMain alttech_domain)
target_include_directories(alttech_topology_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include")
catch_discover_tests(alttech_topology_tests PROPERTIES LABELS unit TIMEOUT 30)

add_executable(alttech_cross_venue_tests unit/test_cross_venue.cpp unit/test_cross_venue_sql.cpp)
target_link_libraries(alttech_cross_venue_tests PRIVATE
    Catch2::Catch2WithMain alttech_cross_venue_core)
target_include_directories(alttech_cross_venue_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
    "${CMAKE_SOURCE_DIR}/src/modules/cross_venue")
catch_discover_tests(alttech_cross_venue_tests PROPERTIES LABELS unit TIMEOUT 30)

add_executable(alttech_global_view_tests unit/test_global_view.cpp)
target_link_libraries(alttech_global_view_tests PRIVATE
    Catch2::Catch2WithMain alttech_cross_venue_core)
target_include_directories(alttech_global_view_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
    "${CMAKE_SOURCE_DIR}/src/modules/cross_venue")
catch_discover_tests(alttech_global_view_tests PROPERTIES LABELS unit TIMEOUT 30)

add_executable(alttech_venue_lite_tests unit/test_venue_lite.cpp)
# Links talib as well: the reference engine's hand-written RSI/ATR are checked
# against the real library, not against a comment claiming they match.
target_link_libraries(alttech_venue_lite_tests PRIVATE
    Catch2::Catch2WithMain alttech_venue_lite_core talib)
target_include_directories(alttech_venue_lite_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
    "${CMAKE_SOURCE_DIR}/src/modules/venue_lite")
catch_discover_tests(alttech_venue_lite_tests PROPERTIES LABELS unit TIMEOUT 30)

foreach(target IN ITEMS
        alttech-service
        alttech-config-check
        alttech-schema
        alttech-nats-setup)
    add_test(NAME "${target}_help" COMMAND $<TARGET_FILE:${target}> --help)
    set_tests_properties("${target}_help" PROPERTIES LABELS unit)
endforeach()

add_test(
    NAME alttech_nats_setup_rejects_invalid_name
    COMMAND $<TARGET_FILE:alttech-nats-setup>
            --url nats://127.0.0.1:1 --input-stream invalid.name)
set_tests_properties(alttech_nats_setup_rejects_invalid_name PROPERTIES
    LABELS unit
    WILL_FAIL TRUE)

add_test(
    NAME alttech_nats_setup_rejects_conflicting_modes
    COMMAND $<TARGET_FILE:alttech-nats-setup>
            --url nats://127.0.0.1:1 --check --apply)
set_tests_properties(alttech_nats_setup_rejects_conflicting_modes PROPERTIES
    LABELS unit
    WILL_FAIL TRUE)

add_test(NAME alttech_version_short COMMAND $<TARGET_FILE:alttech-version> --short)
set_tests_properties(alttech_version_short PROPERTIES LABELS unit)

set(ALTTECH_CONFIG_TEST_ENV
    ALTTECH_ENVIRONMENT=test
    ALTTECH_LOG_LEVEL=info
    ALTTECH_INSTANCE_ID=alttech-test
    ALTTECH_NATS_URL=nats://127.0.0.1:4222
    ALTTECH_NATS_INPUT_STREAM=ALTSPOTTER_BARS
    ALTTECH_NATS_OUTPUT_STREAM=ALTTECH_ANALYTICS
    ALTTECH_NATS_DURABLE=alttech-bars
    ALTTECH_POSTGRES_DSN=postgresql://alttech@127.0.0.1/alttech)

add_test(
    NAME alttech_config_common
    COMMAND ${CMAKE_COMMAND} -E env ${ALTTECH_CONFIG_TEST_ENV}
            $<TARGET_FILE:alttech-config-check>
            --file "${CMAKE_SOURCE_DIR}/config/common.example.yaml" --kind common)
set_tests_properties(alttech_config_common PROPERTIES LABELS unit)

add_test(
    NAME alttech_config_service
    COMMAND ${CMAKE_COMMAND} -E env ${ALTTECH_CONFIG_TEST_ENV}
            $<TARGET_FILE:alttech-config-check>
            --file "${CMAKE_SOURCE_DIR}/config/alttech.example.yaml" --kind service)
set_tests_properties(alttech_config_service PROPERTIES LABELS unit)

add_executable(alttech_macro_tests unit/test_macro_features.cpp)
target_link_libraries(alttech_macro_tests PRIVATE Catch2::Catch2WithMain alttech_macro_core)
target_include_directories(alttech_macro_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
    "${CMAKE_SOURCE_DIR}/src/modules/macro")
catch_discover_tests(alttech_macro_tests PROPERTIES LABELS unit TIMEOUT 60)

add_executable(alttech_dataset_tests unit/test_dataset.cpp)
target_link_libraries(alttech_dataset_tests PRIVATE Catch2::Catch2WithMain alttech_talib_features)
target_include_directories(alttech_dataset_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/include")
catch_discover_tests(alttech_dataset_tests PROPERTIES LABELS unit TIMEOUT 30)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME macro_availability_sql
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/macro_availability_sql.py)
    set_tests_properties(macro_availability_sql PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()
