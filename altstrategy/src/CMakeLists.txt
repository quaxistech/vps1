# src/ — the altstrategy-service implementation (self-contained VPS1 decision
# engine). One binary, one process, one systemd unit.
#
#   core/    foundational modules (no IO policy): common, time, domain, logging,
#            analytics contracts.
#   infra/   IO adapters: config loader, NATS messaging, PostgreSQL persistence.
#   engine/  the adaptive decision engine — pure logic (engine/) + IO adapters
#            and the event-driven orchestrator (engine/io/).
#   app/     composition root: shared PgPool + NatsClient, the DecisionEngine and
#            the internal maintenance scheduler, graceful shutdown.

# --- foundational modules (order = dependency order) ---
add_subdirectory(core/common)
add_subdirectory(core/time)
add_subdirectory(core/logging)
add_subdirectory(core/analytics_contracts)

# --- infrastructure adapters ---
add_subdirectory(infra/config)
add_subdirectory(infra/persistence)
add_subdirectory(infra/messaging)

# --- the adaptive decision engine (pure core + IO/orchestrator) ---
add_subdirectory(engine)

# --- application composition root ---
add_library(altstrategy_app STATIC app/application.cpp)
target_include_directories(altstrategy_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(altstrategy_app PUBLIC
    altstrategy_common altstrategy_config altstrategy_logging altstrategy_messaging
    altstrategy_persistence altstrategy_engine altstrategy_engine_io)

# --- the single service binary ---
# altstrategy_engine carries an INTERFACE --whole-archive wrap so the strategy
# self-registrations survive; it is linked plainly here (transitively via app).
add_executable(altstrategy-service main.cpp)
target_link_libraries(altstrategy-service PRIVATE
    altstrategy_app altstrategy_engine_io altstrategy_engine)
