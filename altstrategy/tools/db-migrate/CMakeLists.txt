add_executable(altstrategy-db-migrate src/main.cpp)
target_link_libraries(altstrategy-db-migrate PRIVATE altstrategy_common altstrategy_logging altstrategy_time PkgConfig::PQXX)
