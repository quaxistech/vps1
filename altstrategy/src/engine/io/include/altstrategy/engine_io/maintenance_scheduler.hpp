#pragma once

// maintenance_scheduler (§37). One internal thread runs the periodic self-
// contained jobs — outcome maturation, stats/posterior refresh, drift scan,
// parameter calibration, training-example materialization, snapshot export and
// retention — none of which requires VPS2. Not a separate service/timer.

#include "altstrategy/engine/allocator.hpp"
#include "altstrategy/engine_io/market_reader.hpp"
#include "altstrategy/engine_io/snapshot_exporter.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <set>
#include <thread>
#include <vector>

namespace altstrategy::messaging { class NatsClient; }

namespace altstrategy::engine_io {

class DecisionStore;

struct SchedulerConfig {
    int64_t outcome_scan_interval_ms = 60'000;
    int64_t posterior_refresh_interval_ms = 300'000;
    int64_t drift_scan_interval_ms = 900'000;
    int64_t snapshot_interval_ms = 3'600'000;
    int64_t retention_interval_ms = 86'400'000;
    bool export_enabled = true;
    std::string label_spec_hash = "altstrategy-labelspec-v1";
    engine::AllocatorConfig allocator{};
    // Epoch (ms) before which outcomes are ignored by ALL learning: posteriors,
    // drift and parameter calibration. Set it whenever the engine changes in a
    // way that invalidates past results — otherwise clearing the derived tables
    // just rebuilds the same verdict from the same history.
    int64_t learning_from_ms = 0;
    int64_t lifecycle_min_samples = 100;
    int64_t lifecycle_cooldown_ms = 6LL * 3600 * 1000;
    // Each strategy's DECLARED starting lifecycle, from the registry.
    //
    // Without it the lifecycle pass assumed ACTIVE for every strategy that has
    // no override row, while the decision path reads the same strategy as its
    // declared default (strategy_engine.cpp). Two readings of one fact: a
    // strategy shipped as RESEARCH was graded SHADOW and never executable on
    // one path, and treated as ACTIVE on the other -- where the ACTIVE branch
    // only demotes, so it could never be promoted on merit, and its demotion
    // was recorded as "from ACTIVE" in an audit trail that never held it.
    std::map<std::string, engine::Lifecycle> strategy_defaults;
    int64_t calibration_window_ms = 7LL * 24 * 3600 * 1000;
    int64_t calibration_min_samples = 30;
    double calibration_learning_rate = 0.3;
};

class MaintenanceScheduler {
public:
    MaintenanceScheduler(DecisionStore& store, MarketReader& reader, SnapshotExporter& exporter,
                         altstrategy::messaging::NatsClient& nats, SchedulerConfig cfg);
    ~MaintenanceScheduler();

    void start();
    void stop();

    // One outcome-maturation pass (also callable from the CLI outcome-rebuild).
    // Returns the number of proposals matured. Self-contained: needs no VPS2.
    int mature_outcomes(int64_t now_ms);
    // Market outcome of every candidate at 15m / 1h / 4h (setup -> model
    // estimate -> what the market did), whatever its lifecycle.
    int record_candidate_market_outcomes(int64_t now_ms);

    // One snapshot export (also the CLI archive-export path).
    bool run_snapshot_export(const std::string& kind, int64_t now_ms);

    // One posterior refresh: matured outcomes -> hierarchical shrunk edges ->
    // altstrategy.strategy_posteriors, which the runtime allocator reads. This
    // is what lets the engine down-weight a strategy or a direction that is
    // losing money; without it adaptive_score can never differ from static_score.
    // Decomposes each resolved outcome into ml/strategy/geometry/cost shares and
    // records what the candidate's other plan variants would have returned.
    int attribute_outcomes(int64_t now_ms);
    int update_posteriors(int64_t now_ms);
    // Baseline-vs-recent metric drift per strategy, persisted as a snapshot.
    int scan_drift(int64_t now_ms);
    // Publishes each strategy's tunable schema and seeds a default active set.
    int bootstrap_parameter_schemas();
    // Moves the active set a bounded step toward what the realised paths say.
    int calibrate_strategy_parameters(int64_t now_ms);

    // Replay resolved proposals' real price paths over a stop/target grid and
    // record the expectancy surface. The only way to compare geometries with the
    // ordering of stop and target hits preserved.
    int calibrate_geometry(int64_t now_ms);

    // One retention pass over every table in altstrategy.retention_policies
    // (also callable from the CLI retention-run path). Bounds on-disk size.
    void run_retention(int64_t now_ms);

private:
    void loop();

    // Cells already written in the current pass (samples repeat per outcome).
    std::set<std::string> seen_;

    DecisionStore& store_;
    MarketReader& reader_;
    SnapshotExporter& exporter_;
    altstrategy::messaging::NatsClient& nats_;
    SchedulerConfig cfg_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    int64_t last_from_sequence_ = 0;
    std::string last_snapshot_id_;
};

} // namespace altstrategy::engine_io
