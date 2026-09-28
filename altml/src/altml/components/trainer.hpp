#pragma once

#include "altml/components/component.hpp"
#include "altml/core/config.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace altml {

// Native in-process trainer: replaces the retired Python offline pipeline.
//
// On first start (no ACTIVE model) it bootstraps from whatever labeled history
// PostgreSQL already holds; afterwards it re-trains on a rolling window at the
// configured cadence. Continual learning without overfitting is achieved by
// construction: every cycle re-fits on the freshest window with a purged
// walk-forward protocol, early stopping on an embargoed validation slice, and
// a champion-challenger gate — a new model replaces the ACTIVE one only when
// it is strictly better on the same held-out recent data. Training is bounded
// (threads, memory estimate, wall-clock budget) and yields to live inference
// under load (deferred with JOB_DEFERRED_RESOURCE_PRESSURE).
class TrainerComponent : public Component {
public:
    explicit TrainerComponent(const config::RuntimeConfig& rt);
    ~TrainerComponent() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "trainer"; }
    [[nodiscard]] Result<void> init() override;
    void run() override;

private:
    // The econ (multi-horizon) schedule; run() delegates to it in econ mode.
    void run_econ_schedule();

    const config::RuntimeConfig& rt_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One synchronous training cycle for `altml train` (diagnostics / controlled
// re-run / CI). `force` skips the schedule and resource-pressure checks; every
// safety and promotion gate still applies. Returns a process exit code.
int run_training_once(const config::RuntimeConfig& rt, bool force);

// `altml replay`: rebuild the archived point-in-time input of every live
// prediction a model made in [from_ms, to_ms), re-score it with the registered
// artifact and calibration, and report how many match. Returns an exit code.
int run_prediction_replay(const config::RuntimeConfig& rt, const std::string& model_id,
                          int64_t from_ms, int64_t to_ms, int64_t limit);

} // namespace altml
