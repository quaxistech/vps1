add_library(altstrategy_config STATIC
    src/config.cpp
    src/service_bootstrap.cpp)
target_include_directories(altstrategy_config PUBLIC include)
target_link_libraries(altstrategy_config
    PUBLIC altstrategy_common yaml-cpp::yaml-cpp
    PRIVATE altstrategy_logging)
