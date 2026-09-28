add_executable(altstrategy-replay src/main.cpp)
target_link_libraries(altstrategy-replay PRIVATE
    altstrategy_engine altstrategy_engine_io altstrategy_persistence altstrategy_common)
install(TARGETS altstrategy-replay RUNTIME DESTINATION bin)
