#include "altstrategy/engine_io/maintenance_scheduler.hpp"

#include "altstrategy/engine/allocator.hpp"
#include "altstrategy/engine/adapt.hpp"
#include "altstrategy/engine/strategy.hpp"
#include "altstrategy/engine/attribution.hpp"
#include "altstrategy/engine/outcomes.hpp"
#include "altstrategy/engine_io/decision_store.hpp"
#include "altstrategy/engine_io/wire_codec.hpp"
#include "altstrategy/logging/logger.hpp"
#include "altstrategy/messaging/nats_client.hpp"
#include "altstrategy/messaging/nats_subjects.hpp"

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace altstrategy::engine_io {

namespace {
// How long after its time stop a proposal's path may still complete. Candles
// settle within minutes and raw trades only ever disappear, so a path still
// incomplete after this never will be: the proposal is recorded as
// DATA_INCOMPLETE, with the reason, instead of being re-read every pass until
// retention removes it.
constexpr int64_t kIncompleteRecordAfterMs = 6LL * 3600 * 1000;
} // namespace

MaintenanceScheduler::MaintenanceScheduler(DecisionStore& store, MarketReader& reader,
                                           SnapshotExporter& exporter,
                                           altstrategy::messaging::NatsClient& nats, SchedulerConfig cfg)
    : store_(store), reader_(reader), exporter_(exporter), nats_(nats), cfg_(std::move(cfg)) {}

MaintenanceScheduler::~MaintenanceScheduler() { stop(); }

void MaintenanceScheduler::start() {
    if (running_.exchange(true)) return;
    stop_requested_.store(false);
    thread_ = std::thread([this] { loop(); });
}

void MaintenanceScheduler::stop() {
    stop_requested_.store(true);
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

int MaintenanceScheduler::mature_outcomes(int64_t now_ms) {
    int matured = 0;
    const auto pending = store_.proposals_awaiting_outcome(now_ms);
    for (const auto& pp : pending) {
        if (stop_requested_.load()) break;   // shutting down: the rest waits for the next start
        OutcomeGeometry g;
        g.side = pp.side; g.entry_type = pp.entry_type; g.entry_price = pp.entry_price;
        g.trigger_price = pp.trigger_price; g.stop_loss_price = pp.stop_loss_price;
        g.take_profit_price = pp.take_profit_price; g.created_at_ms = pp.created_at_ms;
        g.entry_fill_timeout_ms = pp.entry_fill_timeout_ms; g.valid_until_ms = pp.valid_until_ms;
        g.time_stop_at_ms = pp.time_stop_at_ms;
        // Score the trade with the cost it was PLANNED with. Until now this line
        // did not exist, so the struct default of 8.0 scored every outcome while
        // the proposal had been priced at a different figure entirely.
        g.cost_bps = pp.cost_bps;
        // The path over the full maturation window (entry + holding), with
        // its gaps, through evaluator version 2 (QUALITY-04).
        const auto path = reader_.read_price_path(pp.canonical_pair_id, pp.created_at_ms,
                                                  pp.time_stop_at_ms + 60'000);
        if (path.bars.empty()) continue;   // data not ready yet; retry next pass
        auto outcome = reconstruct_outcome_v2(g, path, pp.candidate_id, pp.signal_proposal_id, now_ms);
        // Incomplete now may complete later (a late minute); a proposal past
        // the window the source still holds is recorded as incomplete rather
        // than retried forever -- the reason is on the row.
        if (outcome.outcome == MarketOutcomeType::data_incomplete &&
            now_ms - pp.time_stop_at_ms < kIncompleteRecordAfterMs)
            continue;
        outcome.cost_model_version = pp.cost_model_version;
        outcome.round_trip_cost_bps = pp.cost_bps;
        // The outcome and its publication commit together; the outbox sends
        // it to the stream (JetStream live sync for VPS2).
        EnvelopeMeta m; m.canonical_pair_id = pp.canonical_pair_id; m.event_type = "market_outcome";
        m.message_id = outcome.outcome_id;
        const OutboxMessage publication{
            "market_outcome:" + outcome.outcome_id, "market_outcome",
            altstrategy::nats_subjects::market_outcome(std::to_string(pp.canonical_pair_id)),
            encode_market_outcome(outcome, m), pp.signal_proposal_id, pp.candidate_id, now_ms};
        if (store_.save_market_outcome(outcome, &publication)) {
            store_.mark_proposal_state(pp.signal_proposal_id, ProposalState::outcome_final, "matured", now_ms);
            ++matured;
        }
    }
    if (matured > 0) log::info("outcomes_matured", {{"count", matured}});
    return matured;
}

int MaintenanceScheduler::record_candidate_market_outcomes(int64_t now_ms) {
    int written = 0, unresolved = 0;
    for (const auto& pc : store_.candidates_awaiting_market_outcome(now_ms, 500)) {
        if (stop_requested_.load()) break;
        // One read covers every horizon: the reference bar plus four hours.
        // 1m candles, never the 1-second trade path: raw trades cover the
        // last ~6 h, so a candidate 4 h old used to get 1-second buckets
        // looked up by MINUTE -- 1h and 4h found a missing minute and were
        // skipped for good, and 15m was built from fifteen single seconds.
        const int64_t ref_close = (pc.created_at_ms / 60'000) * 60'000;
        const auto bars = reader_.read_minute_path(pc.canonical_pair_id, ref_close - 60'000,
                                                   ref_close + 241 * 60'000LL);
        const auto outs = engine::candidate_market_outcomes(pc.candidate_id, pc.side,
                                                            pc.created_at_ms, bars, pc.cost_bps);
        if (outs.empty()) { ++unresolved; continue; }
        for (const auto& o : outs) if (store_.save_candidate_market_outcome(o)) ++written;
    }
    if (written > 0 || unresolved > 0)
        log::info("candidate_market_outcomes", {{"written", written}, {"unresolved", unresolved}});
    return written;
}

int MaintenanceScheduler::attribute_outcomes(int64_t now_ms) {
    int attributed = 0, counterfactuals = 0;
    for (const auto& w : store_.load_unattributed()) {
        if (stop_requested_.load()) break;
        if (w.variants.empty()) continue;
        // A regime change is read from our own record: the regime the candidate
        // was raised under versus the regime in force when the trade resolved.
        const bool regime_changed = !w.regime_at_creation.empty() &&
                                    !w.regime_at_resolution.empty() &&
                                    w.regime_at_creation != w.regime_at_resolution;
        for (const auto& v : w.variants) {
            std::vector<MarketOutcome> siblings;
            std::vector<int> sibling_versions;
            for (const auto& o : w.variants) {
                if (o.signal_proposal_id == v.signal_proposal_id) continue;
                siblings.push_back(o.outcome);
                sibling_versions.push_back(o.plan_version);
            }
            const auto cfs = compare_counterfactuals(w.ranking_id, v.outcome, siblings, now_ms,
                                                     sibling_versions);
            // Regret is the same figure for every row of the set: what the best
            // variant in hindsight returned, minus what this one did.
            double regret = 0;
            for (const auto& c : cfs) regret = std::max(regret, c.ranking_regret);
            if (!v.needs_attribution) continue;
            for (const auto& c : cfs) if (store_.save_counterfactual(c)) ++counterfactuals;

            TradeSignalProposal p;
            p.signal_proposal_id = v.signal_proposal_id;
            p.round_trip_cost_bps = v.round_trip_cost_bps;
            p.expected_net_return = v.expected_net_return;
            p.edge_estimated = v.edge_estimated;
            const auto a = attribute_outcome(p, v.outcome, std::nullopt, regret, regime_changed,
                                             now_ms);
            if (store_.save_attribution(a)) ++attributed;
        }
    }
    if (attributed > 0)
        log::info("outcomes_attributed",
                  {{"attributed", attributed}, {"counterfactuals", counterfactuals}});
    return attributed;
}

int MaintenanceScheduler::scan_drift(int64_t now_ms) {
    // Recent = the last day; baseline = the week before it. compute_drift() holds
    // a snapshot at STABLE below 30 filled samples, so a young strategy cannot
    // trip a structural-break verdict on noise.
    constexpr int64_t kDay = 24LL * 3600 * 1000;
    const int64_t split_ms = now_ms - kDay;
    const int64_t from_ms = std::max(now_ms - 8 * kDay, cfg_.learning_from_ms);
    int written = 0, degraded = 0, transitions = 0;
    // Lifecycle is decided from the same evidence as drift, so it is evaluated in
    // the same pass. evaluate_lifecycle() existed and was never called: no
    // strategy could be promoted out of CANDIDATE on merit or demoted out of
    // ACTIVE on evidence, which is why strategy_lifecycle and lifecycle_events
    // were both empty while a STRUCTURAL_BREAK strategy kept trading.
    const auto current = store_.load_lifecycle_overrides();
    const auto posteriors = store_.load_posteriors(cfg_.min_plan_geometry_version);
    for (const auto& [sid, entry] :
         store_.load_drift_inputs(from_ms, split_ms, cfg_.min_plan_geometry_version)) {
        if (stop_requested_.load()) break;
        const auto d = compute_drift(sid, entry.first, entry.second, now_ms);
        if (store_.save_drift(d)) ++written;
        if (d.status == DriftStatus::degraded || d.status == DriftStatus::structural_break) {
            ++degraded;
            log::warn("strategy_drift", {{"strategy", sid},
                                         {"status", to_string(d.status)},
                                         {"outcome_drift", d.outcome_drift},
                                         {"samples", entry.second.sample_count}});
        }

        LifecycleInputs li;
        li.current = engine::current_lifecycle(sid, current, cfg_.strategy_defaults);
        li.drift = d.status;
        li.sample_count = entry.second.sample_count;
        li.min_samples = cfg_.lifecycle_min_samples;
        // The cooldown is enforced by the store: a transition is only recorded
        // when the strategy has not moved inside the window.
        li.cooldown_elapsed =
            store_.lifecycle_cooldown_elapsed(sid, now_ms, cfg_.lifecycle_cooldown_ms);
        if (const auto* post = posteriors.strategy_global(sid)) {
            li.posterior_edge = post->posterior_mean_edge;
            li.posterior_uncertainty = post->posterior_uncertainty;
        }
        if (auto t = evaluate_lifecycle(sid, entry.first, li, now_ms)) {
            if (store_.save_lifecycle_transition(*t)) {
                ++transitions;
                log::info("lifecycle_transition",
                          {{"strategy", sid}, {"from", to_string(t->from_status)},
                           {"to", to_string(t->to_status)}, {"drift", to_string(d.status)},
                           {"edge_bps", li.posterior_edge * 10000.0}});
            }
        }
    }
    if (written > 0)
        log::info("drift_scanned", {{"strategies", written}, {"degraded", degraded},
                                    {"lifecycle_transitions", transitions}});
    return written;
}

int MaintenanceScheduler::bootstrap_parameter_schemas() {
    int published = 0;
    for (const auto& s : engine::StrategyRegistry::instance().create_all()) {
        const auto& m = s->meta();
        store_.ensure_strategy_definition(m);
        if (store_.ensure_parameter_schema(m.strategy_id, m.strategy_version, m.parameter_schema))
            ++published;
    }
    if (published > 0) log::info("parameter_schemas_published", {{"strategies", published}});
    return published;
}

int MaintenanceScheduler::calibrate_strategy_parameters(int64_t now_ms) {
    // calibrate_parameters() existed and was never called, so every strategy ran
    // on its compiled defaults for its whole life. It clamps each value to its
    // schema bound and moves it only a bounded fraction toward the observed
    // target, and it refuses outright below 30 samples — so this is a slow,
    // reversible nudge, not a refit.
    if (!cfg_.calibration_enabled) return 0;
    const int64_t from_ms = std::max(now_ms - cfg_.calibration_window_ms, cfg_.learning_from_ms);
    const auto targets = store_.load_calibration_targets(from_ms, cfg_.min_plan_geometry_version);
    if (targets.empty()) return 0;
    const auto current = store_.load_param_sets();

    int updated = 0;
    for (const auto& s : engine::StrategyRegistry::instance().create_all()) {
        const auto& m = s->meta();
        // A parameter set may only be judged on the trades IT produced. Keying
        // the targets by (strategy, parameter_set) closes the control loop: the
        // 7-day outcome window is dominated by the previous geometry, so scoring
        // a fresh set against it drove the values monotonically toward their
        // schema floor — a 30 % step every 15 minutes, each one measuring a
        // population the current parameters never created.
        const auto cur = current.find(m.strategy_id);
        const std::string set_id =
            cur != current.end() ? cur->second.parameter_set_id : std::string{};
        const auto t = targets.find(m.strategy_id + "|" + set_id);
        if (t == targets.end() || t->second.sample_count < cfg_.calibration_min_samples) continue;

        std::map<std::string, double> base;
        if (cur != current.end() && !cur->second.values.empty()) base = cur->second.values;
        else for (const auto& p : m.parameter_schema) base[p.name] = p.default_value;

        const auto proposed = calibrate_parameters(m.parameter_schema, base, t->second,
                                                   cfg_.calibration_learning_rate);
        if (proposed == base) continue;   // nothing moved: do not churn a new version
        const std::string from_id = set_id.empty() ? std::string{"default"} : set_id;
        if (store_.save_parameter_set(m.strategy_id, m.strategy_version, proposed, from_id, now_ms)) {
            ++updated;
            log::info("parameters_calibrated",
                      {{"strategy", m.strategy_id},
                       {"samples", t->second.sample_count},
                       {"derived_from", from_id}});
        }
    }
    if (updated > 0) log::info("parameter_calibration", {{"strategies", updated}});
    return updated;
}

bool MaintenanceScheduler::run_snapshot_export(const std::string& kind, int64_t now_ms) {
    if (!cfg_.export_enabled) return false;
    auto res = exporter_.export_snapshot(kind, last_from_sequence_, now_ms, last_snapshot_id_);
    if (!res) { log::warn("snapshot_export_failed", {{"kind", kind}}); return false; }
    last_from_sequence_ = res->to_sequence;
    last_snapshot_id_ = res->snapshot_id;
    // Publish the snapshot-created event (metadata only; artifacts move via rsync).
    log::info("snapshot_exported", {{"kind", kind}, {"snapshot_id", res->snapshot_id},
                                    {"signals", res->signal_count}, {"to_sequence", res->to_sequence}});
    // Metadata only: the snapshot itself is durable on disk and in the archive
    // tables whether or not this notice reaches the bus.
    if (!nats_.publish("archive.snapshot.created.v1", res->snapshot_id + "|" + res->manifest_hash + "|" +
                                                          std::to_string(res->to_sequence)))
        log::warn("snapshot_notice_publish_failed", {{"snapshot_id", res->snapshot_id}});
    return true;
}

void MaintenanceScheduler::run_retention(int64_t now_ms) {
    const auto results = store_.run_retention(now_ms);
    int64_t total_deleted = 0;
    for (const auto& r : results) {
        if (!r.ok) {
            log::warn("retention_table_failed", {{"table", r.table_name}, {"message", r.error}});
            continue;
        }
        if (r.deleted_rows > 0) {
            log::info("retention_purged", {{"table", r.table_name}, {"deleted_rows", r.deleted_rows}});
            total_deleted += r.deleted_rows;
        }
    }
    if (total_deleted > 0) log::info("retention_pass_complete", {{"total_deleted_rows", total_deleted}});
}

// Refresh the adaptive posteriors from matured outcomes.
//
// This is the batch half of the adaptive allocator, the piece allocator.hpp
// describes as "the BATCH maths run by the maintenance scheduler". It was never
// wired into the loop, so `altstrategy.strategy_posteriors` stayed empty, every
// candidate carried posterior 0 with an effective sample size of 0, and
// adaptive_score was always exactly static_score. The system could not learn
// that a strategy or a direction was losing money, no matter how many outcomes
// it recorded.
//
// Only RESOLVED outcomes enter the estimate. ENTRY_NOT_REACHED carries no
// position and therefore no return; folding it in as a zero would drag every
// edge toward zero in proportion to how often the entry was simply not filled,
// which is a property of the entry geometry, not of the strategy's edge.
int MaintenanceScheduler::update_posteriors(int64_t now_ms) {
    const auto samples =
        store_.load_outcome_samples(20000, cfg_.learning_from_ms, cfg_.min_plan_geometry_version);
    if (samples.empty()) return 0;

    engine::PosteriorEstimator estimator(cfg_.allocator);

    // Observations are grouped at four tiers of specificity. A thin cell is
    // shrunk toward the tier above it, so a strategy with two lucky wins on one
    // symbol cannot dominate ranking.
    using Obs = std::vector<std::pair<int64_t, double>>;
    std::map<std::string, Obs> exact;    // strategy+regime+group+pair+side+holding
    std::map<std::string, Obs> by_group; // strategy+regime+group
    std::map<std::string, Obs> by_regime;// strategy+regime
    std::map<std::string, Obs> by_strat;  // strategy
    Obs global;

    const auto key = [](std::initializer_list<std::string> parts) {
        std::string k;
        for (const auto& p : parts) { k += p; k += '|'; }
        return k;
    };

    for (const auto& s : samples) {
        const std::string sid = s.strategy_id + "@" + s.strategy_version;
        const std::string side = to_string(s.side);
        const std::string hold = to_string(s.holding_class);
        const auto pair = std::to_string(s.canonical_pair_id);
        exact[key({sid, s.regime, s.symbol_group, pair, side, hold})]
            .emplace_back(s.resolved_at_ms, s.net_return);
        by_group[key({sid, s.regime, s.symbol_group})].emplace_back(s.resolved_at_ms, s.net_return);
        by_regime[key({sid, s.regime})].emplace_back(s.resolved_at_ms, s.net_return);
        by_strat[key({sid})].emplace_back(s.resolved_at_ms, s.net_return);
        global.emplace_back(s.resolved_at_ms, s.net_return);
    }

    const engine::StatCell global_cell = estimator.decay(global, now_ms);
    int written = 0;

    for (const auto& s : samples) {
        const std::string sid = s.strategy_id + "@" + s.strategy_version;
        const std::string side = to_string(s.side);
        const std::string hold = to_string(s.holding_class);
        const auto pair = std::to_string(s.canonical_pair_id);
        const std::string exact_key = key({sid, s.regime, s.symbol_group, pair, side, hold});
        if (!seen_.insert(exact_key).second) continue;   // one row per cell per pass

        const engine::StatCell local = estimator.decay(exact[exact_key], now_ms);
        const std::vector<engine::StatCell> parents = {
            estimator.decay(by_group[key({sid, s.regime, s.symbol_group})], now_ms),
            estimator.decay(by_regime[key({sid, s.regime})], now_ms),
            estimator.decay(by_strat[key({sid})], now_ms),
        };

        engine::StrategyPosterior p = estimator.estimate(s.strategy_id, s.strategy_version, local,
                                                         parents, global_cell.mean_edge);
        p.parameter_set_id = s.parameter_set_id;
        p.regime = s.regime;
        p.symbol_group = s.symbol_group;
        p.canonical_pair_id = s.canonical_pair_id;
        p.side = s.side;
        p.holding_class = s.holding_class;
        p.sample_count = static_cast<int64_t>(exact[exact_key].size());
        p.updated_at_ms = now_ms;
        p.evidence_version = cfg_.min_plan_geometry_version;
        if (store_.save_posterior(p)) ++written;

        // The runtime lookup falls back from the exact cell to progressively
        // more general ones (see PosteriorTable::best), so those tiers have to
        // exist as rows of their own. Writing only exact cells means a candidate
        // in a regime or on a symbol not seen before matches nothing and is
        // never adapted — which is what left adaptive_score identical to
        // static_score even once the exact cells were being persisted.
        const auto write_tier = [&](const std::string& regime, const std::string& group,
                                    const engine::StatCell& cell,
                                    const std::vector<engine::StatCell>& tier_parents) {
            engine::StrategyPosterior g = estimator.estimate(s.strategy_id, s.strategy_version,
                                                             cell, tier_parents,
                                                             global_cell.mean_edge);
            g.parameter_set_id = "*";
            g.regime = regime;
            g.symbol_group = group;
            g.canonical_pair_id = 0;
            g.side = Side::unspecified;
            g.holding_class = HoldingClass::unspecified;
            g.sample_count = static_cast<int64_t>(cell.n);
            g.updated_at_ms = now_ms;
            g.evidence_version = cfg_.min_plan_geometry_version;
            if (store_.save_posterior(g)) ++written;
        };
        const std::string group_key = key({sid, s.regime, s.symbol_group});
        const std::string regime_key = key({sid, s.regime});
        const std::string strat_key = key({sid});
        if (seen_.insert("T:" + group_key).second) {
            write_tier(s.regime, s.symbol_group, parents[0], {parents[1], parents[2]});
        }
        if (seen_.insert("T:" + regime_key).second) {
            write_tier(s.regime, "*", parents[1], {parents[2]});
        }
        if (seen_.insert("T:" + strat_key).second) {
            write_tier("*", "*", parents[2], {});
        }
    }
    seen_.clear();

    log::info("posteriors_refreshed",
              {{"cells", written},
               {"resolved_outcomes", static_cast<int64_t>(samples.size())},
               {"global_edge_bps", global_cell.mean_edge * 10000.0}});
    return written;
}

// Replay every resolved proposal's actual price path against a grid of stop and
// target distances, and report the expectancy surface.
//
// This answers the only question that matters about geometry — "what would have
// made money on the paths the market actually printed" — with the ordering of
// events intact. It cannot be answered from stored MFE/MAE alone: those give the
// magnitudes of the excursions but not which came first, so a hypothetical
// tighter stop cannot be evaluated against them.
//
// Distances are in ATR multiples, which is the unit the planner works in and the
// only one comparable across instruments and price levels.
int MaintenanceScheduler::calibrate_geometry(int64_t now_ms) {
    const auto sample = store_.load_replay_sample();
    if (sample.size() < 50) return 0;   // too thin to calibrate on

    static constexpr double kStopGrid[] = {0.5, 0.75, 1.0, 1.5, 2.0};
    static constexpr double kTargetGrid[] = {0.5, 1.0, 1.5, 2.0, 3.0};
    constexpr double kCostBps = 8.0;    // round-trip fees + spread, as measured

    struct Cell { int n = 0; int wins = 0; double sum_net = 0; };
    std::map<std::string, Cell> grid;   // "side|stop|target" -> stats

    for (const auto& rp : sample) {
        auto path = reader_.read_path(rp.p.canonical_pair_id, rp.p.created_at_ms,
                                      rp.p.time_stop_at_ms + 60'000);
        if (path.bars.size() < 2) continue;
        const double dir = rp.p.side == Side::long_ ? 1.0 : -1.0;
        const double entry = rp.p.entry_type == EntryType::stop_market ||
                                     rp.p.entry_type == EntryType::stop_limit
                                 ? rp.p.trigger_price : rp.p.entry_price;
        if (!(entry > 0) || !(rp.atr_used > 0)) continue;

        for (const double s_mult : kStopGrid) {
            for (const double t_mult : kTargetGrid) {
                OutcomeGeometry g;
                g.side = rp.p.side; g.entry_type = rp.p.entry_type;
                g.entry_price = rp.p.entry_price; g.trigger_price = rp.p.trigger_price;
                g.created_at_ms = rp.p.created_at_ms;
                g.entry_fill_timeout_ms = rp.p.entry_fill_timeout_ms;
                g.valid_until_ms = rp.p.valid_until_ms;
                g.time_stop_at_ms = rp.p.time_stop_at_ms;
                g.stop_loss_price = entry - dir * s_mult * rp.atr_used;
                g.take_profit_price = entry + dir * t_mult * rp.atr_used;

                const auto o = reconstruct_outcome(g, path.bars, path.source, rp.p.candidate_id,
                                                   rp.p.signal_proposal_id, now_ms);
                if (o.outcome == MarketOutcomeType::entry_not_reached ||
                    o.outcome == MarketOutcomeType::data_incomplete) {
                    continue;
                }
                char key[64];
                std::snprintf(key, sizeof(key), "%s|%.2f|%.2f",
                              rp.p.side == Side::long_ ? "long" : "short", s_mult, t_mult);
                Cell& c = grid[key];
                ++c.n;
                if (o.outcome == MarketOutcomeType::target_first) ++c.wins;
                c.sum_net += o.realized_gross_return * 10000.0 - kCostBps;
            }
        }
    }
    if (grid.empty()) return 0;

    std::string json = "{\"grid\":[";
    bool first = true;
    std::string best_key;
    double best_exp = -1e18;
    for (const auto& [k, c] : grid) {
        if (c.n < 20) continue;   // do not rank a cell on a handful of trades
        const double exp_bps = c.sum_net / c.n;
        if (exp_bps > best_exp) { best_exp = exp_bps; best_key = k; }
        if (!first) json += ',';
        first = false;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "{\"cell\":\"%s\",\"n\":%d,\"win_pct\":%.1f,\"expectancy_bps\":%.2f}",
                      k.c_str(), c.n, 100.0 * c.wins / c.n, exp_bps);
        json += buf;
    }
    json += "],\"best_cell\":\"" + best_key + "\",\"best_expectancy_bps\":" +
            std::to_string(best_exp) + "}";

    const std::string run_id = "calib-" + std::to_string(now_ms);
    store_.save_calibration_run(run_id, "stop_target_atr", "{\"unit\":\"atr_multiple\"}", json);
    log::info("geometry_calibrated", {{"sample", static_cast<int64_t>(sample.size())},
                                      {"cells", static_cast<int64_t>(grid.size())},
                                      {"best_cell", best_key},
                                      {"best_expectancy_bps", best_exp}});
    return static_cast<int>(grid.size());
}

void MaintenanceScheduler::loop() {
    bootstrap_parameter_schemas();
    using clock = std::chrono::steady_clock;
    auto next_outcome = clock::now();
    auto next_posterior = clock::now();
    auto next_calibration = clock::now();
    auto next_snapshot = clock::now() + std::chrono::milliseconds(cfg_.snapshot_interval_ms);
    auto next_retention = clock::now() + std::chrono::milliseconds(cfg_.retention_interval_ms);
    bool full_done = false;
    while (running_.load()) {
        const auto now = clock::now();
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count();
        // A pass runs several jobs of seconds each, and it used to run to the end
        // whatever happened: on SIGTERM, stop() joined this thread for the whole
        // pass, the unit's 30 s TimeoutStopSec expired and systemd SIGKILLed the
        // process (2026-09-25 16:35, mid-pass). Every job now yields between
        // steps and inside its per-item loops.
        const auto live = [&] { return running_.load(); };
        try {
            if (live() && now >= next_outcome) {
                mature_outcomes(now_ms);
                if (live()) record_candidate_market_outcomes(now_ms);
                // Attribution consumes exactly what maturation produces, so it
                // shares the cadence rather than needing a timer of its own.
                if (live()) attribute_outcomes(now_ms);
                next_outcome = now + std::chrono::milliseconds(cfg_.outcome_scan_interval_ms);
            }
            if (live() && now >= next_posterior) {
                update_posteriors(now_ms);
                next_posterior = now + std::chrono::milliseconds(cfg_.posterior_refresh_interval_ms);
            }
            if (live() && now >= next_calibration) {
                calibrate_geometry(now_ms);
                if (live()) scan_drift(now_ms);
                if (live()) calibrate_strategy_parameters(now_ms);
                next_calibration = now + std::chrono::milliseconds(cfg_.drift_scan_interval_ms);
            }
            if (live() && now >= next_snapshot) {
                run_snapshot_export(full_done ? "INCREMENTAL" : "FULL", now_ms);
                full_done = true;
                next_snapshot = now + std::chrono::milliseconds(cfg_.snapshot_interval_ms);
            }
            if (live() && now >= next_retention) {
                run_retention(now_ms);
                next_retention = now + std::chrono::milliseconds(cfg_.retention_interval_ms);
            }
        } catch (const std::exception& e) {
            // A maintenance failure never takes the live pipeline down (§38).
            log::error("maintenance_job_exception", {{"message", e.what()}});
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace altstrategy::engine_io
