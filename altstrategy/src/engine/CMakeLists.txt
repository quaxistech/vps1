# altstrategy_engine — the altstrategy.v1 adaptive decision engine.
#
# Pure decision logic (decision_types, strategy catalogue, ensemble, allocator,
# ranking, trade plan, proposal, outcomes, attribution, adaptive state) plus the
# IO adapters (v2 codec, decision_store, AltML evaluator) and the event-driven
# orchestrator. The pure layers link only altstrategy_common and are fully unit-testable.

add_library(altstrategy_engine STATIC
    decision_types.cpp
    ml_evaluation.cpp
    strategy_registry.cpp
    strategies/trend.cpp
    strategies/breakout.cpp
    strategies/vwap_cross.cpp
    strategies/reversion_crowding.cpp
    strategies/microstructure_flow.cpp
    strategies/basis_dislocation.cpp
    strategy_engine.cpp
    economic_policy.cpp
    signal_predictions.cpp
    state_candidates.cpp
    ensemble.cpp
    allocator.cpp
    ranking.cpp
    trade_plan.cpp
    proposal.cpp
    outcomes.cpp
    attribution.cpp
    adapt.cpp)

target_include_directories(altstrategy_engine PUBLIC include)
target_link_libraries(altstrategy_engine PUBLIC altstrategy_common altstrategy_time)

# The compile-time strategy self-registrations are static initializers that a
# linker would otherwise strip from a static archive. Wrap ONLY this archive in
# --whole-archive at every link site (INTERFACE), so consumers link it plainly
# and still keep the whole catalogue. Same pattern the former strategy lib used.
target_link_options(altstrategy_engine INTERFACE
    -Wl,--whole-archive $<TARGET_FILE:altstrategy_engine> -Wl,--no-whole-archive)

# --- IO adapters (wire + DB + orchestrator) ---
add_subdirectory(io)
