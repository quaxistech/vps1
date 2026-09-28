add_library(altstrategy_messaging STATIC
    src/nats_client.cpp)

target_include_directories(altstrategy_messaging PUBLIC include)
target_link_libraries(altstrategy_messaging
    PUBLIC altstrategy_common altstrategy_time altstrategy_proto nats_static
    PRIVATE altstrategy_logging)
