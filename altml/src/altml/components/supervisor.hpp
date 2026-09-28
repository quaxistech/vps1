#pragma once

#include "altml/components/component.hpp"
#include "altml/core/error.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace altml {

// Runs every registered component in one process: sequential fail-fast init,
// then one thread per component. The global ShutdownToken is the single stop
// mechanism — a failed component stops the whole service (exit code 1) so
// systemd Restart=on-failure restarts everything together.
class Supervisor {
public:
    void add(std::unique_ptr<Component> component);

    // Sequential init in registration order; stops at the first failure.
    [[nodiscard]] Result<void> init_all();

    // Spawns component threads, signals READY, blocks until shutdown, joins.
    // Returns the process exit code (0 clean stop, 1 component failure).
    int run_all();

private:
    std::vector<std::unique_ptr<Component>> components_;
    std::atomic<bool> failed_{false};
};

} // namespace altml
