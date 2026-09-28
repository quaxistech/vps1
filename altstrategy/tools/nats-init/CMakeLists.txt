add_executable(altstrategy-nats-init src/main.cpp)
target_link_libraries(altstrategy-nats-init PRIVATE altstrategy_logging nats_static)
