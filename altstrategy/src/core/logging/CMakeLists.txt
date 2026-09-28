add_library(altstrategy_logging STATIC src/logger.cpp)
target_include_directories(altstrategy_logging PUBLIC include)
target_link_libraries(altstrategy_logging
    PUBLIC nlohmann_json::nlohmann_json altstrategy_time
    PRIVATE spdlog::spdlog fmt::fmt PkgConfig::SYSTEMD)
