#pragma once
// Per-venue analytics sweeps.
//
// Each primary-venue module also drives the sweep for the output it owns, so
// the same analytics exist for every venue the cross-venue engine needs. The
// sweep blocks until process shutdown and must run on its own thread. A missing
// alttech schema is fatal: the sweep fails closed rather than silently
// producing nothing.

#include "venue_store.hpp"

#include <functional>
#include <thread>

namespace YAML {
class Node;
}

namespace alttech::messaging {
class NatsClient;
}

namespace alttech::venuelite {

// Run one per-venue sweep of `kind` (feature / regime / microstructure /
// structure) until shutdown, driven by the owning module's config node.
void run_venue_sweep(persistence::PgPool& pool, messaging::NatsClient& nats,
                     store::VenueOutput kind, const YAML::Node& module_config);

// Joins a sweep thread on scope exit. The sweep always exits once the global
// shutdown token is set — which module failure (mark_failed) also triggers — so
// this never blocks past process teardown regardless of the module's exit path.
struct SweepThreadJoiner {
    std::thread thread;
    SweepThreadJoiner() = default;
    explicit SweepThreadJoiner(std::thread t) : thread(std::move(t)) {}
    SweepThreadJoiner(const SweepThreadJoiner&) = delete;
    SweepThreadJoiner& operator=(const SweepThreadJoiner&) = delete;
    ~SweepThreadJoiner() {
        if (thread.joinable()) thread.join();
    }
};

} // namespace alttech::venuelite
