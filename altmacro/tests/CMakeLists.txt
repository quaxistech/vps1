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
