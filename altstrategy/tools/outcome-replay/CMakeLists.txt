# Offline recomputation of archived plans and outcomes under the current plan
# geometry and both outcome evaluators, from an exported dataset (QUALITY-05).
add_executable(altstrategy-outcome-replay src/main.cpp)
target_link_libraries(altstrategy-outcome-replay PRIVATE altstrategy_engine altstrategy_common)
install(TARGETS altstrategy-outcome-replay RUNTIME DESTINATION bin)
