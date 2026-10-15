#include "app/application.hpp"

#include "altstrategy/common/readiness.hpp"
#include "altstrategy/common/shutdown.hpp"
#include "altstrategy/config/config.hpp"
#include "altstrategy/engine/strategy.hpp"
#include "altstrategy/engine_io/decision_engine.hpp"
#include "altstrategy/engine_io/decision_store.hpp"
#include "altstrategy/engine_io/maintenance_scheduler.hpp"
#include "altstrategy/engine_io/market_reader.hpp"
#include "altstrategy/engine_io/outbox_publisher.hpp"
#include "altstrategy/engine_io/snapshot_exporter.hpp"
#include "altstrategy/engine_io/wire_codec.hpp"
#include "altstrategy/logging/logger.hpp"
#include "altstrategy/messaging/nats_client.hpp"
#include "altstrategy/persistence/pg.hpp"

#include <pqxx/pqxx>

#include <chrono>
#include <string>
#include <utility>

using namespace altstrategy;

namespace altstrategy::app {

namespace {
YAML::Node section(const YAML::Node& service, const std::string& key) {
    if (service && service[key]) return service[key];
    return YAML::Node(YAML::NodeType::Map);
}
std::vector<std::string> str_list(const YAML::Node& n, const std::string& key,
                                  std::vector<std::string> def) {
    if (n && n[key] && n[key].IsSequence()) {
        std::vector<std::string> out;
        for (const auto& e : n[key]) out.push_back(e.as<std::string>());
        return out;
    }
    return def;
}

// Read a pinned AltTech feature set. `positions` may be given directly as
// {engine_name: producer_index}, or as an ordered `names` list when the
// consumer reads the whole vector (the cross-venue set is only 30 members, so
// listing it in order is clearer than listing 30 indices).
engine_io::PositionalFeatureSet positional_set(const YAML::Node& parent, const std::string& key) {
    engine_io::PositionalFeatureSet set;
    const auto n = parent && parent[key] ? parent[key] : YAML::Node(YAML::NodeType::Undefined);
    if (!n || !n.IsMap()) return set;   // unconfigured => every vector refused
    set.name = config::opt_str(n, "name", "");
    set.version = static_cast<int32_t>(config::opt_int(n, "version", 0));
    set.schema_hash = config::opt_str(n, "schema_hash", "");
    set.value_count = static_cast<size_t>(config::opt_int(n, "value_count", 0));

    if (n["names"] && n["names"].IsSequence()) {
        size_t i = 0;
        for (const auto& e : n["names"]) set.positions[e.as<std::string>()] = i++;
        if (set.value_count == 0) set.value_count = i;
    }
    if (n["positions"] && n["positions"].IsMap()) {
        for (const auto& kv : n["positions"]) {
            set.positions[kv.first.as<std::string>()] = static_cast<size_t>(kv.second.as<int64_t>());
        }
    }
    return set;
}
} // namespace

Application::Application(config::ServiceContext ctx) : ctx_(std::move(ctx)) {}

Application::~Application() = default;

bool Application::acquire_singleton_lock() {
    try {
        singleton_lock_ = std::make_unique<pqxx::connection>(ctx_.common.postgres_dsn);
        pqxx::nontransaction lock_txn(*singleton_lock_);
        const bool acquired =
            lock_txn.exec1("SELECT pg_try_advisory_lock(hashtextextended('altstrategy-service', 0))")[0]
                .as<bool>();
        if (!acquired) { log::critical("altstrategy_instance_already_running", {}); return false; }
    } catch (const std::exception& e) {
        log::critical("altstrategy_singleton_lock_failed", {{"message", e.what()}});
        return false;
    }
    return true;
}

// The singleton lock is a session-level advisory lock, so it lives exactly as
// long as its connection. A PostgreSQL restart ends that session: the service
// kept running WITHOUT the lock (verified on a scratch cluster 2026-09-26: after
// a kill -9 of the postmaster and a restart, pg_locks held no advisory lock),
// and a second instance could then start beside it and publish every signal
// twice. Checked on every housekeeping tick: a dead lock session is replaced by
// a fresh one; if another instance took the lock meanwhile, this one stops
// (fail closed: exit 1, systemd restarts it behind the holder).
void Application::verify_singleton_lock() {
    bool alive = false;
    try {
        if (singleton_lock_ && singleton_lock_->is_open()) {
            pqxx::nontransaction t(*singleton_lock_);
            alive = t.exec1("SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND granted "
                            "AND pid = pg_backend_pid()")[0].as<int64_t>() > 0;
        }
    } catch (const std::exception&) {
        alive = false;
    }
    if (alive) return;
    try {
        auto conn = std::make_unique<pqxx::connection>(ctx_.common.postgres_dsn);
        pqxx::nontransaction t(*conn);
        const bool acquired =
            t.exec1("SELECT pg_try_advisory_lock(hashtextextended('altstrategy-service', 0))")[0].as<bool>();
        if (!acquired) {
            log::critical("altstrategy_singleton_lock_lost",
                          {{"message", "another instance holds the lock; stopping"}});
            exit_code_.store(1);
            ShutdownToken::instance().request_stop();
            return;
        }
        singleton_lock_ = std::move(conn);
        log::warn("altstrategy_singleton_lock_reacquired", {});
    } catch (const std::exception& e) {
        // The database is unreachable: nothing can be written anyway, and the
        // lock is retried on the next tick.
        log::warn("altstrategy_singleton_lock_unverifiable", {{"message", e.what()}});
    }
}

bool Application::setup() {
    auto nats = messaging::NatsClient::connect(ctx_.common.nats_url, ctx_.service_name);
    if (!nats) { log::critical("nats_unavailable", {{"message", nats.error().message}}); return false; }
    nats_ = std::move(*nats);

    const auto pool_size = static_cast<size_t>(config::opt_int(ctx_.service, "postgres_pool_size", 12));
    pool_ = std::make_unique<persistence::PgPool>(ctx_.common.postgres_dsn, pool_size);
    if (auto probe = pool_->with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); }); !probe) {
        log::critical("postgres_unavailable", {{"message", probe.error().message}});
        return false;
    }
    if (!acquire_singleton_lock()) return false;

    // --- assemble the self-contained decision engine ---
    const YAML::Node ctx_cfg = section(ctx_.service, "context");
    const YAML::Node out_cfg = section(ctx_.service, "outcomes");
    const YAML::Node exp_cfg = section(ctx_.service, "training_snapshots");

    engine_io::MarketReaderConfig rcfg;
    rcfg.bitget_exchange_id = static_cast<int>(config::opt_int(out_cfg, "bitget_exchange_id", 1));
    rcfg.candles_table = config::opt_str(out_cfg, "candles_table", "market_candles_1m");
    rcfg.trades_table = config::opt_str(out_cfg, "trades_table", "market_trades_raw");
    rcfg.prefer_raw_trades = config::opt_bool(out_cfg, "prefer_raw_trades", true);
    rcfg.instrument_specs_table =
        config::opt_str(out_cfg, "instrument_specs_table", rcfg.instrument_specs_table);
    rcfg.pair_map_table = config::opt_str(out_cfg, "pair_map_table", rcfg.pair_map_table);

    const YAML::Node pub_cfg = section(ctx_.service, "publication");
    engine_io::OutboxPublisherConfig pcfg;
    pcfg.ack_timeout_ms = config::opt_int(pub_cfg, "ack_timeout_ms", pcfg.ack_timeout_ms);
    pcfg.max_age_ms = config::opt_int(pub_cfg, "max_age_hours", pcfg.max_age_ms / 3600000) * 3600000;

    engine_io::ExporterConfig ecfg;
    ecfg.export_root = config::opt_str(exp_cfg, "export_root", "/var/lib/altstrategy/exports/signal-quality");
    ecfg.format = config::opt_str(exp_cfg, "format", "csv");


    engine_io::DecisionEngineConfig dcfg;
    dcfg.assembler.bitget_features = positional_set(ctx_cfg, "bitget_feature_set");
    dcfg.assembler.cross_venue_features = positional_set(ctx_cfg, "cross_venue_feature_set");
    dcfg.assembler.microstructure_features = positional_set(ctx_cfg, "microstructure_feature_set");
    dcfg.assembler.max_microstructure_age_ms =
        config::opt_int(ctx_cfg, "max_microstructure_age_sec", 120) * 1000;
    dcfg.assembler.microstructure_window =
        config::opt_str(ctx_cfg, "microstructure_window",
                        dcfg.assembler.microstructure_window);
    dcfg.assembler.decision_timeframe_minutes =
        static_cast<int32_t>(config::opt_int(ctx_cfg, "decision_timeframe_minutes",
                                             dcfg.assembler.decision_timeframe_minutes));
    // Higher grids admitted as CONTEXT, distinct from the execution grid above.
    // Absent = execution grid only, which is what this service did before.
    if (const auto node = ctx_cfg["context_timeframes_minutes"]; node && node.IsSequence()) {
        dcfg.assembler.context_timeframes_minutes.clear();
        for (const auto& item : node) {
            if (item.IsScalar()) {
                dcfg.assembler.context_timeframes_minutes.push_back(item.as<int32_t>());
            }
        }
    }
    dcfg.assembler.max_context_age_ms =
        config::opt_int(ctx_cfg, "max_context_age_sec", 0) * 1000;
    // --- previously inert config -------------------------------------------
    // Every block below was present in the YAML and read by nobody: the structs
    // are default-constructed members, so editing the file changed nothing. The
    // live allocator is the sharpest case — the batch posterior job was
    // configured while the allocator that actually weights candidates kept a
    // 30-day half-life.
    const YAML::Node alloc_cfg = section(ctx_.service, "adaptive_allocator");
    dcfg.allocator.prior_strength =
        config::opt_double(alloc_cfg, "prior_strength", dcfg.allocator.prior_strength);
    dcfg.allocator.edge_scale = config::opt_double(alloc_cfg, "edge_scale", dcfg.allocator.edge_scale);
    dcfg.allocator.weight_min = config::opt_double(alloc_cfg, "weight_min", dcfg.allocator.weight_min);
    dcfg.allocator.weight_max = config::opt_double(alloc_cfg, "weight_max", dcfg.allocator.weight_max);
    dcfg.allocator.half_life_days =
        config::opt_double(alloc_cfg, "half_life_days", dcfg.allocator.half_life_days);
    dcfg.allocator.weight_gain = config::opt_double(alloc_cfg, "weight_gain", dcfg.allocator.weight_gain);

    dcfg.assembler.context_ttl_ms =
        config::opt_int(ctx_cfg, "context_ttl_sec", dcfg.assembler.context_ttl_ms / 1000) * 1000;
    dcfg.assembler.max_component_age_ms =
        config::opt_int(ctx_cfg, "max_component_age_sec",
                        dcfg.assembler.max_component_age_ms / 1000) * 1000;

    // One freshness bound for AltML's econ estimates, used by both the
    // assembler (admission to a context) and the economic policy (at the
    // moment of decision), so the two cannot disagree about "fresh".
    const int64_t forecast_age_ms = config::opt_int(ctx_cfg, "max_forecast_age_sec", 180) * 1000;
    if (forecast_age_ms < 60'000 || forecast_age_ms > 900'000) {
        log::critical("invalid_config", {{"key", "context.max_forecast_age_sec"}});
        return false;
    }
    dcfg.assembler.max_forecast_age_ms = forecast_age_ms;
    dcfg.strategy.econ.max_forecast_age_ms = forecast_age_ms;

    const YAML::Node strat_cfg = section(ctx_.service, "strategies");
    dcfg.strategy.min_rr = config::opt_double(strat_cfg, "min_candidate_rr", dcfg.strategy.min_rr);
    dcfg.strategy.candidate_ttl_ms =
        config::opt_int(strat_cfg, "candidate_ttl_sec", dcfg.strategy.candidate_ttl_ms / 1000) * 1000;
    // Full-state decisions: on by default -- the model is not to be limited to
    // the states a detector happened to describe.
    dcfg.state.enabled = config::opt_bool(strat_cfg, "state_candidates", dcfg.state.enabled);
    dcfg.state.candidate_ttl_ms = dcfg.strategy.candidate_ttl_ms;

    const YAML::Node plan_cfg = section(ctx_.service, "trade_plan");
    dcfg.plan.min_stop_bps = config::opt_double(plan_cfg, "min_stop_bps", dcfg.plan.min_stop_bps);
    dcfg.plan.default_max_slippage_bps =
        config::opt_double(plan_cfg, "default_max_slippage_bps", dcfg.plan.default_max_slippage_bps);
    dcfg.plan.max_entry_timeout_ms =
        config::opt_int(plan_cfg, "max_entry_timeout_sec",
                        dcfg.plan.max_entry_timeout_ms / 1000) * 1000;
    // max_holding_hours may only LOWER the ceiling, never raise it.
    //
    // This read had no upper bound, so `max_holding_hours: 200` in YAML would
    // have set a 200-hour ceiling and every downstream std::min() would have
    // honoured it -- the "hard 48h invariant" was hard everywhere except at the
    // one place it was configured. A config asserting something the system
    // forbids is a startup failure, not a value to quietly clamp.
    {
        const int64_t configured_ms =
            config::opt_int(plan_cfg, "max_holding_hours",
                            engine::kMaxHoldingMs / 3600000) * 3600000;
        if (configured_ms > engine::kMaxHoldingMs) {
            log::error("config_rejected",
                       {{"key", "trade_plan.max_holding_hours"},
                        {"configured_hours", configured_ms / 3600000},
                        {"ceiling_hours", engine::kMaxHoldingMs / 3600000},
                        {"reason", "exceeds the absolute holding ceiling"}});
            return false;
        }
        dcfg.plan.extended_max_ms = configured_ms;
    }
    // QUALITY-05: which plan geometry learning may draw on. Read before the
    // engine exists, because the engine loads posteriors by it too.
    const YAML::Node learn_cfg = section(ctx_.service, "learning");
    const int min_geometry = static_cast<int>(
        config::opt_int(learn_cfg, "min_plan_geometry_version", dcfg.min_plan_geometry_version));
    if (min_geometry < 1 || min_geometry > engine::kPlanGeometryVersion) {
        log::error("config_rejected", {{"key", "learning.min_plan_geometry_version"},
                                       {"value", min_geometry},
                                       {"max", engine::kPlanGeometryVersion}});
        return false;
    }
    dcfg.min_plan_geometry_version = min_geometry;
    log::info("engine_configured",
              {{"half_life_days", dcfg.allocator.half_life_days},
               {"min_plan_geometry_version", min_geometry},
               {"min_stop_bps", dcfg.plan.min_stop_bps},
               {"max_holding_hours", dcfg.plan.extended_max_ms / 3600000},
               {"candidate_ttl_sec", dcfg.strategy.candidate_ttl_ms / 1000},
               {"context_ttl_sec", dcfg.assembler.context_ttl_ms / 1000}});

    dcfg.alttech_subjects = str_list(section(ctx_.service, "inputs"), "alttech_subjects",
                                     dcfg.alttech_subjects);
    dcfg.prediction_subjects = str_list(section(ctx_.service, "inputs"), "prediction_subjects",
                                        dcfg.prediction_subjects);
    dcfg.universe_subjects = str_list(section(ctx_.service, "inputs"), "universe_subjects",
                                      dcfg.universe_subjects);

    store_ = std::make_unique<engine_io::DecisionStore>(*pool_);
    // Forward only: the schema must already be the one this binary writes.
    // Versions are four-digit and zero-padded, so they compare as text.
    const auto migrated = store_->newest_schema_migration();
    if (!migrated || *migrated < engine_io::kRequiredSchemaMigration) {
        log::critical("schema_not_migrated",
                      {{"required", std::string(engine_io::kRequiredSchemaMigration)},
                       {"applied", migrated ? (migrated->empty() ? std::string("no ledger") : *migrated)
                                            : std::string("unreadable")},
                       {"message", "run altstrategy-db-migrate before starting this release"}});
        return false;
    }
    reader_ = std::make_unique<engine_io::MarketReader>(*pool_, rcfg);
    exporter_ = std::make_unique<engine_io::SnapshotExporter>(*pool_, ecfg);
    publisher_ = std::make_unique<engine_io::OutboxPublisher>(*store_, *nats_, pcfg);
    engine_ = std::make_unique<engine_io::DecisionEngine>(*nats_, *store_, *publisher_, dcfg);
    engine_->set_price_tick_source([reader = reader_.get()] { return reader->read_price_ticks(); });
    engine_->set_session_seed_source([reader = reader_.get()](int64_t pair, int64_t from, int64_t to) {
        std::vector<engine_io::SessionMinute> out;
        for (const auto& m : reader->read_volume_minutes(pair, from, to))
            out.push_back({m.open_ms, m.quote_volume, m.base_volume});
        return out;
    });
    // The scheduler was constructed from a DEFAULT SchedulerConfig, so every key
    // under `adaptive_allocator` and `maintenance` in the YAML was inert: the
    // Bayesian shrinkage, the decay half-life and the job cadences all silently
    // used their header defaults. Editing the file changed nothing.
    engine_io::SchedulerConfig scfg;
    scfg.allocator = dcfg.allocator;   // one parse, one source of truth
    scfg.allocator.prior_strength =
        config::opt_double(alloc_cfg, "prior_strength", scfg.allocator.prior_strength);
    scfg.allocator.edge_scale = config::opt_double(alloc_cfg, "edge_scale", scfg.allocator.edge_scale);
    scfg.allocator.weight_min = config::opt_double(alloc_cfg, "weight_min", scfg.allocator.weight_min);
    scfg.allocator.weight_max = config::opt_double(alloc_cfg, "weight_max", scfg.allocator.weight_max);
    scfg.allocator.half_life_days =
        config::opt_double(alloc_cfg, "half_life_days", scfg.allocator.half_life_days);
    scfg.allocator.weight_gain = config::opt_double(alloc_cfg, "weight_gain", scfg.allocator.weight_gain);

    const YAML::Node maint_cfg = section(ctx_.service, "maintenance");
    scfg.outcome_scan_interval_ms =
        config::opt_int(maint_cfg, "outcome_scan_interval_ms", scfg.outcome_scan_interval_ms);
    scfg.posterior_refresh_interval_ms =
        config::opt_int(maint_cfg, "posterior_refresh_interval_ms", scfg.posterior_refresh_interval_ms);
    scfg.drift_scan_interval_ms =
        config::opt_int(maint_cfg, "drift_scan_interval_ms", scfg.drift_scan_interval_ms);
    scfg.snapshot_interval_ms =
        config::opt_int(maint_cfg, "snapshot_interval_ms", scfg.snapshot_interval_ms);
    scfg.retention_interval_ms =
        config::opt_int(maint_cfg, "retention_interval_ms", scfg.retention_interval_ms);
    scfg.export_enabled = config::opt_bool(maint_cfg, "export_enabled", scfg.export_enabled);
    const YAML::Node lc_cfg = section(ctx_.service, "lifecycle");
    scfg.learning_from_ms = config::opt_int(learn_cfg, "from_ms", scfg.learning_from_ms);
    scfg.min_plan_geometry_version = min_geometry;
    // `calibration:` was in the YAML and read by nobody.
    const YAML::Node cal_cfg = section(ctx_.service, "calibration");
    scfg.calibration_enabled = config::opt_bool(cal_cfg, "enabled", scfg.calibration_enabled);
    scfg.calibration_min_samples =
        config::opt_int(cal_cfg, "min_samples", scfg.calibration_min_samples);
    scfg.lifecycle_min_samples =
        config::opt_int(lc_cfg, "min_samples", scfg.lifecycle_min_samples);
    scfg.lifecycle_cooldown_ms =
        config::opt_int(lc_cfg, "cooldown_hours", scfg.lifecycle_cooldown_ms / 3600000) * 3600000;
    log::info("allocator_configured",
              {{"prior_strength", scfg.allocator.prior_strength},
               {"half_life_days", scfg.allocator.half_life_days},
               {"lifecycle_min_samples", scfg.lifecycle_min_samples},
               {"learning_from_ms", scfg.learning_from_ms},
               {"lifecycle_cooldown_h", scfg.lifecycle_cooldown_ms / 3600000},
               {"calibration_enabled", scfg.calibration_enabled},
               {"calibration_min_samples", scfg.calibration_min_samples},
               {"edge_scale", scfg.allocator.edge_scale}});

    // The lifecycle pass must read the same starting status the decision path
    // reads, so a strategy with no override row is evaluated as what it ships
    // as, not as ACTIVE.
    for (const auto& s : engine::StrategyRegistry::instance().create_all()) {
        scfg.strategy_defaults[s->meta().strategy_id] = s->meta().default_lifecycle;
    }

    scheduler_ = std::make_unique<engine_io::MaintenanceScheduler>(
        *store_, *reader_, *exporter_, *nats_, scfg);

    // Counted from the registry, not written down. This was the literal 10 and
    // stayed 10 after the catalogue grew to 16, so the startup log asserted a
    // catalogue size the binary did not have.
    log::info("altstrategy_engine_ready",
              {{"postgres_pool_size", pool_size},
               {"strategies", static_cast<int64_t>(
                                  engine::StrategyRegistry::instance().create_all().size())},
               {"self_contained", true}});
    return true;
}

int Application::run() {
    // Optional VPS2 execution feedback (§21): consumed when present, never required.
    if (auto sub = nats_->subscribe("signal.execution_feedback.>", [this](messaging::Msg m) {
            try {
                if (auto fb = engine_io::parse_execution_feedback(m.data())) store_->save_execution_feedback(*fb);
            } catch (const std::exception& e) { log::error("feedback_exception", {{"message", e.what()}}); }
        }); sub) {
        feedback_sub_ = std::make_unique<messaging::Subscription>(std::move(*sub));
    }

    // The publisher first: what an earlier run committed and never sent goes
    // out before anything new is decided.
    publisher_->start();
    if (!engine_->start()) { log::critical("engine_start_failed", {}); return 1; }
    scheduler_->start();

    notify_ready();
    notify_status("altstrategy self-contained decision engine running (event-driven, no order execution)");
    log::info("service_started", {});

    // Report latency percentiles periodically until shutdown.
    auto& shutdown = ShutdownToken::instance();
    // The loop wakes twice per watchdog interval so a single missed tick does
    // not kill a healthy service, and the ping is sent from HERE -- if this loop
    // stops running, the service is not healthy whatever else is still alive.
    while (!shutdown.stop_requested()) {
        if (shutdown.wait_for(std::chrono::seconds(30))) break;
        notify_watchdog();
        verify_singleton_lock();
        if (shutdown.stop_requested()) break;
        engine_->report_latency();
        engine_->report_strategy_funnel();
    }

    notify_stopping();
    scheduler_->stop();
    // Anything still unsent stays in the outbox for the next start.
    publisher_->stop();
    if (nats_) nats_->drain(3000);
    log::info("service_stopped", {{"exit_code", exit_code_.load()}});
    return exit_code_.load();
}

} // namespace altstrategy::app
