add_library(altstrategy_persistence STATIC
    src/pg.cpp)

target_include_directories(altstrategy_persistence PUBLIC include)
target_link_libraries(altstrategy_persistence
    PUBLIC altstrategy_common altstrategy_time altstrategy_logging PkgConfig::PQXX)
