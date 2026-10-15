# altstrategy_engine_io — the IO adapters that bind the pure engine to the wire
# and the database: the wire codec, context assembler, decision store (persistence
# + signal archive), AltML evaluator, market reader, event-driven orchestrator,
# maintenance scheduler and snapshot exporter.

add_library(altstrategy_engine_io STATIC
    wire_codec.cpp
    context_assembler.cpp
    decision_store.cpp
    market_reader.cpp
    snapshot_exporter.cpp
    context_replay.cpp
    maintenance_scheduler.cpp
    outbox_publisher.cpp
    decision_engine.cpp)

target_include_directories(altstrategy_engine_io PUBLIC include)
target_link_libraries(altstrategy_engine_io PUBLIC
    altstrategy_engine altstrategy_proto altstrategy_messaging altstrategy_persistence
    altstrategy_config altstrategy_logging altstrategy_common altstrategy_time
    nlohmann_json::nlohmann_json)
