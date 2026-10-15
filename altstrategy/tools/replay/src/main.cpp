// altstrategy-replay — run the compiled catalogue over archived decision
// contexts and score it against the realised price path.
//
// Why this exists
// ---------------
// Every claim about a strategy in this system has, until now, come from either
// a unit test (which proves the code does what it says, not that what it says
// is profitable) or from live outcomes (which take weeks per hypothesis and
// only exist for strategies that already fire). Neither can answer "is this
// hypothesis worth deploying" before deploying it.
//
// The replay uses ONLY what the decision had at T0: the archived context
// snapshot. The ML forecast is deliberately absent from those snapshots, so a
// replay measures the catalogue WITHOUT ML enrichment -- which is stated in
// every report rather than left for a reader to discover.
//
// Point-in-time
// -------------
// Contexts are replayed in event-time order. The outcome of a candidate is
// resolved from bars that start at or after its creation, through the same
// reconstruct_outcome() the live tracker uses, so a replayed win and a live win
// mean the same thing. No parameter is fitted inside a fold's own test window.

#include "altstrategy/engine/outcomes.hpp"
#include "altstrategy/engine/strategy_engine.hpp"
#include "altstrategy/engine/trade_plan.hpp"
#include "altstrategy/engine_io/context_replay.hpp"
#include "altstrategy/engine_io/market_reader.hpp"
#include "altstrategy/persistence/pg.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <iomanip>
#include <iostream>
#include <limits>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <string>
#include <vector>

using namespace altstrategy;
using namespace altstrategy::engine;

namespace {

struct Args {
    std::string dsn;
    std::string from = "2000-01-01";
    std::string to = "2100-01-01";
    std::string mode = "replay";        // replay | walkforward | ablation
    std::string only;                   // one strategy id, empty = all
    int folds = 4;
    int limit = 0;                      // 0 = no limit
    double cost_bps = 16.0;             // round-trip: fees + spread + slippage
};

// One replayed decision: which strategy, when, and what it earned.
struct Trade {
    std::string strategy_id;
    std::string family;
    int64_t event_time_ms = 0;
    double net_bps = 0;
    double gross_bps = 0;
    bool win = false;
    bool resolved = false;              // false = ambiguous or no path
};

[[noreturn]] void usage() {
    std::cerr <<
        "usage: altstrategy-replay --dsn DSN [options]\n"
        "  --from YYYY-MM-DD      first context date (inclusive)\n"
        "  --to   YYYY-MM-DD      last context date (exclusive)\n"
        "  --mode replay|walkforward|ablation|venues|grids\n""                         venues: Bitget alone vs Bitget + each information venue\n""                         grids:  execution grid alone vs + each higher timeframe\n"
        "  --only STRATEGY_ID     restrict to one strategy\n"
        "  --folds N              walk-forward folds (default 4)\n"
        "  --limit N              stop after N contexts\n"
        "  --cost-bps X           round-trip cost charged to every trade (default 16)\n";
    std::exit(2);
}

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (k == "--dsn") a.dsn = next();
        else if (k == "--from") a.from = next();
        else if (k == "--to") a.to = next();
        else if (k == "--mode") a.mode = next();
        else if (k == "--only") a.only = next();
        else if (k == "--folds") a.folds = std::stoi(next());
        else if (k == "--limit") a.limit = std::stoi(next());
        else if (k == "--cost-bps") a.cost_bps = std::stod(next());
        else if (k == "--help" || k == "-h") usage();
        else usage();
    }
    if (a.dsn.empty()) {
        if (const char* env = std::getenv("ALTSTRATEGY_POSTGRES_DSN")) a.dsn = env;
    }
    if (a.dsn.empty()) usage();
    if (a.folds < 2) a.folds = 2;
    return a;
}

// An ablation blanks one input group from the context before the strategies see
// it, so the difference in measured edge is attributable to that group. It
// REMOVES rather than zeroes: a strategy that requires the input then declines,
// which is the honest counterfactual ("we did not have this") rather than
// "we had it and it read zero".
void ablate(DecisionContext& c, const std::string& what) {
    if (what == "none") return;

    // Component arms: remove one input group entirely.
    if (what == "cross_venue") { c.cross_venue_features.values.clear(); c.cross_venue_features.valid = false; }
    else if (what == "microstructure") { c.microstructure.values.clear(); c.microstructure.valid = false; }
    else if (what == "reference") c.reference_features.clear();
    else if (what == "multi_timeframe") c.context_features.clear();
    else if (what == "regime") { c.global_regime = {}; c.bitget_regime = {}; }

    // Venue arms: keep Bitget (always the target venue) plus a named subset of
    // the information venues. Bitget is never ablated -- it is the venue the
    // trade would execute on, so removing it does not describe any deployable
    // configuration.
    else if (what.rfind("venues:", 0) == 0) {
        const std::string keep = what.substr(7);
        const auto wanted = [&](int32_t id) {
            if (keep == "all") return true;
            if (keep == "bitget") return false;          // information venues dropped
            if (keep == "binance") return id == 2;
            if (keep == "okx") return id == 3;
            if (keep == "bybit") return id == 4;
            return false;
        };
        for (auto it = c.reference_features.begin(); it != c.reference_features.end();)
            it = wanted(it->first) ? std::next(it) : c.reference_features.erase(it);
        // The cross-venue consensus is computed FROM the information venues, so
        // a venue subset that no longer matches it cannot keep it: consensus
        // over four venues is not evidence about a two-venue configuration.
        if (keep != "all") { c.cross_venue_features.values.clear(); c.cross_venue_features.valid = false; }
    }

    // Timeframe arms: keep the execution grid plus a named subset of the higher
    // grids. The execution grid is never ablated -- it is what the decision is
    // taken on.
    else if (what.rfind("grids:", 0) == 0) {
        const std::string keep = what.substr(6);
        if (keep == "exec") { c.context_features.clear(); return; }
        if (keep == "all") return;
        const int32_t only = std::atoi(keep.c_str());
        for (auto it = c.context_features.begin(); it != c.context_features.end();)
            it = (it->first == only) ? std::next(it) : c.context_features.erase(it);
    }
}


struct Summary {
    int trades = 0, wins = 0, unresolved = 0, no_fill = 0;
    double sum_bps = 0;        // net, after the round-trip cost
    double sum_gross_bps = 0;  // before it
    // Kept per trade so the BASELINE can state a median and a profit factor, not
    // just a mean: with samples this small a single outlier moves a mean by more
    // than the entire edge under discussion.
    std::vector<double> gross_bps_samples;
    std::vector<double> net_bps_samples;
    double gross_profit_bps = 0;   // sum of winners, gross
    double gross_loss_bps = 0;     // sum of |losers|, gross
    double sum_holding_min = 0;

    [[nodiscard]] double mean_bps() const { return trades ? sum_bps / trades : 0.0; }
    [[nodiscard]] double mean_gross_bps() const { return trades ? sum_gross_bps / trades : 0.0; }
    [[nodiscard]] double win_rate() const { return trades ? 100.0 * wins / trades : 0.0; }
    [[nodiscard]] double mean_holding_min() const {
        return trades ? sum_holding_min / trades : 0.0;
    }
    // Undefined rather than infinite when nothing lost: a profit factor with no
    // denominator is not a large number, it is an unanswered question.
    [[nodiscard]] double profit_factor() const {
        return gross_loss_bps > 0 ? gross_profit_bps / gross_loss_bps
                                  : std::numeric_limits<double>::quiet_NaN();
    }
    [[nodiscard]] static double median(std::vector<double> v) {
        if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
        std::sort(v.begin(), v.end());
        const size_t n = v.size();
        return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    }
    [[nodiscard]] double median_gross_bps() const { return median(gross_bps_samples); }
    [[nodiscard]] double median_net_bps() const { return median(net_bps_samples); }
    // Worst peak-to-trough run of the net series, in bps, in the order trades
    // were taken. Not a portfolio drawdown -- there is no position sizing here --
    // but the same statement about sequencing.
    [[nodiscard]] double max_drawdown_bps() const {
        double peak = 0, equity = 0, worst = 0;
        for (const double x : net_bps_samples) {
            equity += x;
            peak = std::max(peak, equity);
            worst = std::min(worst, equity - peak);
        }
        return -worst;
    }
};

void print_table(const std::string& title, const std::map<std::string, Summary>& by_key) {
    std::cout << "\n== " << title << "\n";
    // Gross and net side by side. A strategy with a positive gross and a
    // negative net has an edge the cost model eats; one with a negative gross
    // has no edge at all. Those are different findings and the report must not
    // collapse them.
    std::cout << std::left << std::setw(38) << "key" << std::right << std::setw(8) << "trades"
              << std::setw(8) << "win%" << std::setw(11) << "gross avg" << std::setw(11)
              << "gross med" << std::setw(10) << "net avg" << std::setw(10) << "net med"
              << std::setw(8) << "PF" << std::setw(9) << "maxDD" << std::setw(9) << "hold m"
              << std::setw(9) << "no-fill" << "\n";
    // Ordered by measured edge, worst last: the ranking is the finding.
    std::vector<std::pair<std::string, Summary>> rows(by_key.begin(), by_key.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.mean_bps() > b.second.mean_bps(); });
    for (const auto& [k, s] : rows) {
        std::cout << std::left << std::setw(38) << k << std::right << std::setw(8) << s.trades
                  << std::setw(8) << std::fixed << std::setprecision(1) << s.win_rate()
                  << std::setw(11) << std::setprecision(2) << s.mean_gross_bps()
                  << std::setw(11) << s.median_gross_bps()
                  << std::setw(10) << s.mean_bps()
                  << std::setw(10) << s.median_net_bps()
                  << std::setw(8) << s.profit_factor()
                  << std::setw(9) << s.max_drawdown_bps()
                  << std::setw(9) << std::setprecision(0) << s.mean_holding_min()
                  << std::setw(9) << s.no_fill << "\n";
    }
    if (rows.empty()) std::cout << "  (no trades)\n";
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse(argc, argv);

    persistence::PgPool pool(args.dsn, 2);
    engine_io::MarketReaderConfig mrc;
    engine_io::MarketReader reader(pool, mrc);

    // Load the archived contexts in event-time order.
    struct Row {
        engine_io::SnapshotScalars s;
        std::string snapshot;
    };
    std::vector<Row> rows;
    {
        std::string sql =
            "SELECT canonical_pair_id, bitget_instrument_id, context_revision, event_time_ms, "
            "available_at_ms, data_quality, global_universe_status, feature_manifest_hash, "
            "venue_mask, missing_venue_mask, regime_divergence, "
            "COALESCE(fees_bps,4), COALESCE(funding_rate,0), COALESCE(mark_price,0), "
            // Unobserved book scalars are NULL (observed_scalars); the live
            // engine priced them at its stand-ins (context_assembler.cpp:
            // kFallbackSpreadBps 10, kFallbackTopDepthUsdt 5000), so the replay
            // must too, or it would cost an unobserved book at zero.
            "COALESCE(spread_bps,10.0), COALESCE(top_depth_usdt,5000.0), COALESCE(liquidity_rank,0), "
            "COALESCE(expected_slippage_bps,0), COALESCE(spread_observed,false), "
            "COALESCE(depth_observed,false), snapshot::text "
            "FROM altstrategy.decision_context_snapshots "
            "WHERE created_at >= $1::date AND created_at < $2::date "
            "ORDER BY event_time_ms";
        if (args.limit > 0) sql += " LIMIT " + std::to_string(args.limit);
        auto loaded = pool.with_txn([&](pqxx::work& tx) {
        for (const auto& r : tx.exec_params(sql, args.from, args.to)) {
            Row row;
            row.s.canonical_pair_id = r[0].as<int64_t>();
            row.s.bitget_instrument_id = r[1].as<int64_t>();
            row.s.context_revision = r[2].as<int64_t>();
            row.s.event_time_ms = r[3].as<int64_t>();
            row.s.available_at_ms = r[4].as<int64_t>();
            row.s.data_quality = r[5].as<std::string>();
            row.s.universe_status = r[6].as<std::string>();
            row.s.feature_manifest_hash = r[7].as<std::string>();
            row.s.venue_mask = static_cast<uint32_t>(r[8].as<int64_t>());
            row.s.missing_venue_mask = static_cast<uint32_t>(r[9].as<int64_t>());
            row.s.regime_divergence = r[10].as<bool>();
            row.s.fees_bps = r[11].as<double>();
            row.s.funding_rate = r[12].as<double>();
            row.s.mark_price = r[13].as<double>();
            row.s.spread_bps = r[14].as<double>();
            row.s.top_depth_usdt = r[15].as<double>();
            row.s.liquidity_rank = r[16].as<double>();
            row.s.expected_slippage_bps = r[17].as<double>();
            row.s.spread_observed = r[18].as<bool>();
            row.s.depth_observed = r[19].as<bool>();
            row.snapshot = r[20].as<std::string>();
            rows.push_back(std::move(row));
        }
        });
        if (!loaded) {
            std::cerr << "could not read decision_context_snapshots: "
                      << loaded.error().message << "\n";
            return 1;
        }
    }
    std::cout << "contexts loaded: " << rows.size() << "  window: " << args.from << " .. "
              << args.to << "\n";
    if (rows.empty()) {
        std::cout << "nothing to replay\n";
        return 0;
    }

    // The ablations to run. "none" is the baseline every other arm is compared
    // against, and it is always first so the report reads as a difference.
    std::vector<std::string> arms = {"none"};
    if (args.mode == "ablation")
        arms = {"none", "cross_venue", "microstructure", "reference", "multi_timeframe", "regime"};
    else if (args.mode == "venues")
        arms = {"venues:all", "venues:bitget", "venues:binance", "venues:okx", "venues:bybit"};
    else if (args.mode == "grids")
        arms = {"grids:all", "grids:exec", "grids:5", "grids:15", "grids:30",
                "grids:60", "grids:240", "grids:1440"};

    std::map<std::string, std::map<std::string, Summary>> per_arm;   // arm -> strategy -> summary
    std::vector<Trade> all;                                        // baseline arm only

    StrategyEngine eng;
    PlanConfig plan_cfg;

    // Price paths are cached by (instrument, window) and shared across ablation
    // arms. Without this each arm re-reads the path from a database AltSpotter
    // is still writing, so an outcome that was data_incomplete for the baseline
    // resolves for a later arm -- and the arms stop being comparable. The first
    // ablation run showed exactly that: arms differed by a handful of trades in
    // strategies that do not read the ablated input at all.
    std::map<std::tuple<int64_t, int64_t, int64_t>, engine_io::PathResult> path_cache;
    int skipped_unparsable = 0, skipped_no_path = 0;

    for (const auto& arm : arms) {
        // The same one-open-hypothesis rule production applies (HypothesisBook):
        // a persisting setup is replayed as the events it publishes.
        HypothesisBook book;
        std::map<std::string, int64_t> repeats, emitted;
        int64_t contexts_seen = 0;
        for (const auto& row : rows) {
            auto ctx = engine_io::context_from_snapshot(row.snapshot, row.s);
            if (!ctx) { if (arm == "none") ++skipped_unparsable; continue; }
            if (arm != "none") ablate(*ctx, arm);

            const int64_t t0 = ctx->event_time_ms;
            const double px = ctx->last_price();
            book.observe(ctx->canonical_pair_id, px);
            const auto admit = [&](const std::string& id, Side side, int64_t horizon_ms) {
                const bool ok = book.admit(ctx->canonical_pair_id, id, side, t0, horizon_ms, px, ctx->atr()).admitted;
                if (!ok) ++repeats[id];
                return ok;
            };
            const auto candidates = eng.run(*ctx, t0, {}, nullptr, admit);
            if (arm == "none") ++contexts_seen;
            for (const auto& c : candidates) {
                if (arm == "none") ++emitted[c.strategy_id];
                if (!args.only.empty() && c.strategy_id != args.only) continue;
                const auto plans = generate_plans(c, *ctx, t0, plan_cfg);
                if (plans.empty()) continue;
                const auto& v = plans.front();   // the strategy's own declared geometry

                OutcomeGeometry g;
                g.side = c.side;
                g.entry_type = v.entry_type;
                g.entry_price = v.entry_price;
                g.trigger_price = v.trigger_price;
                g.stop_loss_price = v.stop_loss_price;
                g.take_profit_price = v.take_profit_price;
                g.created_at_ms = t0;
                g.entry_fill_timeout_ms = v.entry_fill_timeout_ms;
                g.valid_until_ms = t0 + v.max_holding_time_ms;
                g.time_stop_at_ms = t0 + v.max_holding_time_ms;
                g.cost_bps = args.cost_bps;

                const auto key = std::tuple{ctx->bitget_instrument_id, t0,
                                            g.time_stop_at_ms + 60'000};
                auto cached = path_cache.find(key);
                if (cached == path_cache.end()) {
                    cached = path_cache.emplace(key, reader.read_path(std::get<0>(key),
                                                                     std::get<1>(key),
                                                                     std::get<2>(key))).first;
                }
                const auto& path = cached->second;
                if (path.bars.empty()) { if (arm == "none") ++skipped_no_path; continue; }
                const auto outcome = reconstruct_outcome(g, path.bars, path.source,
                                                         c.candidate_id, "", g.time_stop_at_ms);

                Summary& sum = per_arm[arm][c.strategy_id];
                // An ambiguous outcome is a real production state, not a
                // failure to measure: when stop and target both sit inside one
                // 1m bar the order is unknowable. Counting it either way would
                // manufacture a result, so it is counted separately.
                const bool resolved = !outcome.ambiguous &&
                                      outcome.outcome != MarketOutcomeType::unspecified &&
                                      outcome.outcome != MarketOutcomeType::ambiguous &&
                                      outcome.outcome != MarketOutcomeType::data_incomplete;
                if (!resolved) { ++sum.unresolved; continue; }
                // A decision whose entry never filled is not a trade with a
                // zero return -- it is the absence of a trade. Pooling those in
                // pulls every mean toward zero (flattering a losing strategy)
                // and counts a non-event as a loss in the win rate. They are
                // reported in their own column instead.
                if (outcome.outcome == MarketOutcomeType::entry_not_reached ||
                    outcome.outcome == MarketOutcomeType::entry_expired ||
                    outcome.outcome == MarketOutcomeType::cancelled) {
                    ++sum.no_fill;
                    continue;
                }
                const double net_bps = outcome.realized_net_return * 10'000.0;
                const double gross_bps = outcome.realized_gross_return * 10'000.0;
                ++sum.trades;
                sum.sum_bps += net_bps;
                sum.sum_gross_bps += gross_bps;
                sum.net_bps_samples.push_back(net_bps);
                sum.gross_bps_samples.push_back(gross_bps);
                if (gross_bps >= 0) sum.gross_profit_bps += gross_bps;
                else sum.gross_loss_bps += -gross_bps;
                sum.sum_holding_min += static_cast<double>(outcome.holding_time_ms) / 60'000.0;
                if (net_bps > 0) ++sum.wins;
                if (arm == "none")
                    all.push_back({c.strategy_id, to_string(c.strategy_family), t0, net_bps,
                                   outcome.realized_gross_return * 10'000.0, net_bps > 0, true});
            }
        }
        if (arm == "none") {
            // How often each setup is an EVENT: signals published (after the
            // one-open-hypothesis rule) and repeats of an open one, per context.
            std::cout << "== events per strategy (contexts " << contexts_seen << ")\n";
            std::set<std::string> ids;
            for (const auto& [id, n] : emitted) ids.insert(id);
            for (const auto& [id, n] : repeats) ids.insert(id);
            for (const auto& id : ids) {
                const int64_t e = emitted.count(id) ? emitted.at(id) : 0;
                const int64_t r = repeats.count(id) ? repeats.at(id) : 0;
                std::printf("%-36s published %6lld  (%5.2f%% of contexts)  repeats suppressed %6lld\n",
                            id.c_str(), static_cast<long long>(e),
                            contexts_seen ? 100.0 * static_cast<double>(e) / static_cast<double>(contexts_seen) : 0.0,
                            static_cast<long long>(r));
            }
        }
    }

    std::cout << "unparsable snapshots: " << skipped_unparsable
              << "   candidates without a price path: " << skipped_no_path << "\n";
    std::cout << "cost charged per trade: " << args.cost_bps << " bps (round trip)\n";
    std::cout << "NOTE: archived snapshots carry no ML forecast, so this measures the\n"
                 "      catalogue WITHOUT ML enrichment.\n";

    if (args.mode == "ablation" || args.mode == "venues" || args.mode == "grids") {
        for (const auto& arm : arms) {
            Summary total;
            for (const auto& [_, s] : per_arm[arm]) {
                total.trades += s.trades; total.wins += s.wins;
                total.sum_bps += s.sum_bps; total.sum_gross_bps += s.sum_gross_bps;
                total.gross_profit_bps += s.gross_profit_bps;
                total.gross_loss_bps += s.gross_loss_bps;
                total.sum_holding_min += s.sum_holding_min;
                total.unresolved += s.unresolved; total.no_fill += s.no_fill;
            }
            std::cout << "\n-- ablation arm: " << std::left << std::setw(18) << arm
                      << " trades=" << total.trades
                      << " win%=" << std::fixed << std::setprecision(1) << total.win_rate()
                      << " gross_bps=" << std::setprecision(2) << total.mean_gross_bps()
                      << " net_bps=" << total.mean_bps() << "\n";
        }
        print_table("per strategy, no ablation", per_arm["none"]);
        return 0;
    }

    if (args.mode == "walkforward") {
        // Contiguous equal-count folds in EVENT TIME. Each fold's summary is a
        // pure out-of-sample statement about a fixed parameter set: nothing is
        // fitted here, so this measures stability, not tuned performance. A
        // strategy whose sign flips between folds has no edge worth deploying
        // regardless of its pooled mean.
        std::sort(all.begin(), all.end(),
                  [](const Trade& a, const Trade& b) { return a.event_time_ms < b.event_time_ms; });
        const size_t per = all.size() / static_cast<size_t>(args.folds);
        if (per == 0) {
            std::cout << "\ntoo few resolved trades (" << all.size() << ") for "
                      << args.folds << " folds\n";
            return 0;
        }
        std::map<std::string, std::vector<double>> fold_means;   // strategy -> mean per fold
        for (int k = 0; k < args.folds; ++k) {
            const size_t lo = per * static_cast<size_t>(k);
            const size_t hi = (k == args.folds - 1) ? all.size() : lo + per;
            std::map<std::string, Summary> fold;
            for (size_t i = lo; i < hi; ++i) {
                Summary& s = fold[all[i].strategy_id];
                ++s.trades; s.sum_bps += all[i].net_bps;
                s.sum_gross_bps += all[i].gross_bps; s.wins += all[i].win ? 1 : 0;
                s.net_bps_samples.push_back(all[i].net_bps);
                s.gross_bps_samples.push_back(all[i].gross_bps);
                if (all[i].gross_bps >= 0) s.gross_profit_bps += all[i].gross_bps;
                else s.gross_loss_bps += -all[i].gross_bps;
            }
            print_table("fold " + std::to_string(k + 1) + " of " + std::to_string(args.folds),
                        fold);
            for (const auto& [id, s] : fold) fold_means[id].push_back(s.mean_bps());
        }
        std::cout << "\n== stability across folds (sign consistency is the test)\n";
        for (const auto& [id, means] : fold_means) {
            int pos = 0;
            for (double m : means) pos += m > 0 ? 1 : 0;
            std::cout << std::left << std::setw(38) << id << std::right
                      << " folds_positive=" << pos << "/" << means.size() << "\n";
        }
        return 0;
    }

    print_table("per strategy", per_arm["none"]);
    std::map<std::string, Summary> by_family;
    for (const auto& r : all) {
        Summary& s = by_family[r.family];
        ++s.trades; s.sum_bps += r.net_bps; s.sum_gross_bps += r.gross_bps;
        s.wins += r.win ? 1 : 0;
    }
    print_table("per family", by_family);
    return 0;
}
