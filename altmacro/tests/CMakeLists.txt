# Unit and contract tests.
#
# Everything here runs without a network, a database or a broker. The paths
# that genuinely need those — a real bootstrap, restart recovery, provider
# failure and database failure — are driven by tests/shell against a running
# instance, because a mock of a provider's wire format would only be testing
# the mock, and the wire format is exactly what has to be right.

add_executable(altmacro_tests
    unit/test_dukascopy_decode.cpp
    unit/test_dukascopy_paths.cpp
    unit/test_validation.cpp
    unit/test_calendar.cpp
    unit/test_fred_parse.cpp
    unit/test_frankfurter_symbol.cpp
    unit/test_publisher_subjects.cpp
    unit/test_config.cpp
    unit/test_resume.cpp)
target_link_libraries(altmacro_tests PRIVATE altmacro_core Catch2::Catch2WithMain)
target_compile_definitions(altmacro_tests PRIVATE
    ALTMACRO_VERSION="${PROJECT_VERSION}"
    ALTMACRO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

include(Catch)
catch_discover_tests(altmacro_tests)

add_executable(altmacro_http_probe tools/http_probe.cpp)
target_link_libraries(altmacro_http_probe PRIVATE altmacro_core)
add_library(altmacro_stall_dns SHARED tools/stall_dns.c)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME controlled_https
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/controlled_https.py
            $<TARGET_FILE:altmacro_http_probe> $<TARGET_FILE:altmacro_stall_dns>)
set_tests_properties(controlled_https PROPERTIES TIMEOUT 45)

add_executable(altmacro_publication_probe tools/publication_probe.cpp)
target_link_libraries(altmacro_publication_probe PRIVATE altmacro_core)
add_test(NAME controlled_publication
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/controlled_publication.py
            $<TARGET_FILE:altmacro_publication_probe>)
set_tests_properties(controlled_publication PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
