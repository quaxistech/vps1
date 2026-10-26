#pragma once

#include "alttech/common/error.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alttech::application {

struct ModuleFailure {
    std::string module;
    alttech::Error error;
};

// Coordinates the configured in-process analytics modules. A module reports ready
// only after its external resources have been validated and its subscriptions
// are active. The first failure stops the complete process.
class ModuleSupervisor {
public:
    explicit ModuleSupervisor(std::size_t expected_modules);

    void mark_ready(std::string_view module);
    void mark_failed(std::string_view module, const alttech::Error& error);

    // Waits until every module is ready, one fails, or process shutdown begins.
    [[nodiscard]] bool wait_until_started();
    [[nodiscard]] std::optional<ModuleFailure> failure() const;
    [[nodiscard]] std::size_t ready_count() const;

    // Liveness beacon. A module calls heartbeat() from its steady-state loop with
    // the maximum time it may legitimately take before looping again (a generous
    // multiple of its own cycle plus the query bound). all_alive() is false once
    // any beaconing module misses that deadline — the signal the main loop uses
    // to stop feeding the systemd watchdog, so a wedged worker (not just a dead
    // process) is force-restarted. A module that never beacons is not judged.
    void heartbeat(std::string_view module, int64_t valid_for_ms);
    [[nodiscard]] bool all_alive() const;

    // Every module whose lease has expired, with how long ago. A withheld
    // watchdog ping that says only "a module missed its liveness deadline"
    // names no suspect, and the journal then shows nothing but systemd's own
    // kill line -- which is what made 42 kills over four days take a forensic
    // reconstruction to attribute. Report the culprit at the moment of withholding.
    [[nodiscard]] std::vector<std::pair<std::string, int64_t>> stalled_modules() const;

private:
    const std::size_t expected_modules_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::set<std::string, std::less<>> ready_modules_;
    std::optional<ModuleFailure> failure_;
    std::map<std::string, int64_t, std::less<>> liveness_deadline_ms_;
};

} // namespace alttech::application
