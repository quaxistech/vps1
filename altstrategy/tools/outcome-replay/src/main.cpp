// altstrategy-outcome-replay -- recompute archived plans and their market
// outcomes under the CURRENT plan geometry and BOTH outcome evaluators, from an
// exported dataset, without a database (QUALITY-04 / QUALITY-05).
//
// Why this exists
// ---------------
// A market_outcomes row records what one evaluator said about one geometry.
// When either changes, the history it was learned from has to be recomputed
// side by side -- never UPDATEd in place -- and every effect kept apart: the
// path source, the evaluator and the geometry each move the statistics on
// their own. Per archived proposal this tool reports
//
//   v1c   the recorded geometry, evaluator 1, on 1m candles
//   v2c   the recorded geometry, evaluator 2, on 1m candles
//   g2    the geometry the current generate_plans() builds from the
//         candidate's archived T0 inputs (hints, ATR, spread, ML prediction,
//         the venue's tick at T0), evaluator 2, on 1m candles
//
// next to the outcome as recorded. Only inputs archived at T0 enter g2; a
// proposal whose candidate, context or prediction is gone from the archive
// (14-day retention) gets no g2, and says so.
//
// Raw trades are retained for hours, so a historical replay is a 1m-candle
// replay. Coarser than the 1s path most live outcomes used: more AMBIGUOUS,
// reported as such rather than resolved by assumption.
//
//   altstrategy-outcome-replay --dir DIR --out FILE.csv [--g2-time-stop-horizons K]
//
// --g2-time-stop-horizons K (research, DESIGN-01): end the g2 plan K x the
// hypothesis' own expected horizon after its creation when that is earlier
// than the holding class cap the live plan uses.
//
// DIR holds the export (d1_proposals_outcomes.csv, d2_candidates.csv,
// d3_candidate_predictions.csv, d4_contexts.csv, d5_candles.csv,
// d6_specs.csv) produced by the read-only queries kept with it.

#include "altstrategy/engine/cost_model.hpp"
#include "altstrategy/engine/outcomes.hpp"
#include "altstrategy/engine/signal_predictions.hpp"
#include "altstrategy/engine/trade_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace altstrategy::engine;

namespace {

constexpr int64_t kMin = 60'000;
// g2_rebuilt + 10 geometry columns + 7 outcome columns + g2_cost_bps.
constexpr size_t kG2Columns = 19;

// ------------------------------------------------------------------- CSV in

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (quoted) {
            if (ch == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
            else if (ch == '"') quoted = false;
            else cur += ch;
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += ch;
        }
    }
    out.push_back(std::move(cur));
    return out;
}

// Every row of a headed CSV, as name -> value. A file that is missing is an
// error, not an empty table.
using Row = std::unordered_map<std::string, std::string>;
void read_csv(const std::string& path, const std::function<void(const Row&)>& fn) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string line;
    if (!std::getline(in, line)) return;
    const auto header = split_csv(line);
    Row row;
    while (std::getline(in, line)) {
        // A quoted field may span lines (descriptors JSON never does; be safe).
        while (std::count(line.begin(), line.end(), '"') % 2 == 1) {
            std::string more;
            if (!std::getline(in, more)) break;
            line += "\n" + more;
        }
        const auto f = split_csv(line);
        row.clear();
        for (size_t i = 0; i < header.size() && i < f.size(); ++i) row[header[i]] = f[i];
        fn(row);
    }
}

double num(const Row& r, const std::string& k, double def = NAN) {
    const auto it = r.find(k);
    if (it == r.end() || it->second.empty()) return def;
    return std::stod(it->second);
}
int64_t i64(const Row& r, const std::string& k, int64_t def = 0) {
    const auto it = r.find(k);
    if (it == r.end() || it->second.empty()) return def;
    return std::stoll(it->second);
}
const std::string& str(const Row& r, const std::string& k) {
    static const std::string empty;
    const auto it = r.find(k);
    return it == r.end() ? empty : it->second;
}
bool flag(const Row& r, const std::string& k) { return str(r, k) == "t" || str(r, k) == "true"; }

// ------------------------------------------------------------------- inputs

struct Candles {
    // pair -> minute open -> bar
    std::unordered_map<int64_t, std::map<int64_t, PathBar>> by_pair;

    // The 1m path over [from, to) and its gaps: every missing minute.
    PricePath path(int64_t pair, int64_t from_ms, int64_t to_ms) const {
        PricePath p;
        p.source = OutcomePath::m1_candles;
        p.bar_ms = kMin;
        const auto it = by_pair.find(pair);
        for (int64_t t = from_ms / kMin * kMin; t < to_ms; t += kMin) {
            const PathBar* b = nullptr;
            if (it != by_pair.end()) {
                const auto m = it->second.find(t);
                if (m != it->second.end()) b = &m->second;
            }
            if (b) p.bars.push_back(*b);
            else p.gaps.emplace_back(t, t + kMin);
        }
        p.covered_until_ms = to_ms;
        return p;
    }
};

struct Inputs {
    Candles candles;
    std::unordered_map<std::string, Row> candidates, contexts;
    std::unordered_map<std::string, std::vector<HorizonPrediction>> predictions;
    PriceTickTable ticks;
};

HorizonPrediction prediction_of(const Row& r) {
    HorizonPrediction h;
    h.horizon_minutes = static_cast<int32_t>(i64(r, "horizon_minutes"));
    const std::string s = str(r, "state");
    h.state = s == "PRESENT" ? PredictionState::present : s == "INCOMPLETE" ? PredictionState::incomplete
            : s == "STALE" ? PredictionState::stale : s == "ABSTAIN" ? PredictionState::abstain
            : s == "INCOMPATIBLE" ? PredictionState::incompatible : PredictionState::missing;
    h.model_id = str(r, "model_id");
    h.role = str(r, "model_role");
    h.p_up = num(r, "p_up", 0); h.p_flat = num(r, "p_flat", 0); h.p_down = num(r, "p_down", 0);
    h.expected_return_bps = num(r, "expected_return_bps", 0);
    const double up = num(r, "expected_up_excursion_bps"), down = num(r, "expected_down_excursion_bps");
    h.has_excursions = std::isfinite(up) && std::isfinite(down);
    if (h.has_excursions) { h.expected_up_excursion_bps = up; h.expected_down_excursion_bps = down; }
    h.bar_open_time_ms = i64(r, "bar_open_time_ms");
    return h;
}

Side side_of(const std::string& s) { return s == "long" ? Side::long_ : s == "short" ? Side::short_ : Side::unspecified; }
EntryType entry_of(const std::string& s) { return entry_from_string(s); }

// The candidate and its context rebuilt from what was archived at T0.
bool rebuild(const Inputs& in, const std::string& candidate_id, StrategyCandidate& c, DecisionContext& ctx,
             const Row& proposal) {
    const auto ci = in.candidates.find(candidate_id);
    if (ci == in.candidates.end()) return false;
    const Row& cr = ci->second;
    const auto xi = in.contexts.find(str(cr, "context_id"));
    if (xi == in.contexts.end()) return false;
    const Row& xr = xi->second;
    ctx.context_id = str(xr, "context_id");
    ctx.canonical_pair_id = i64(xr, "canonical_pair_id");
    ctx.event_time_ms = i64(xr, "event_time_ms");
    ctx.mark_price = num(xr, "mark_price", 0);
    ctx.spread_bps = num(xr, "spread_bps", 10.0);
    ctx.expected_slippage_bps = num(xr, "expected_slippage_bps", 0);
    ctx.top_depth_usdt = num(xr, "top_depth_usdt", 5'000.0);
    ctx.liquidity_rank = num(xr, "liquidity_rank", 0);
    ctx.spread_observed = flag(xr, "spread_observed");
    ctx.depth_observed = flag(xr, "depth_observed");
    const double atr = num(xr, "atr_14");
    if (std::isfinite(atr)) ctx.bitget_features.values["atr_14"] = atr;
    const double intensity = num(xr, "trade_intensity");
    if (std::isfinite(intensity)) ctx.microstructure.values["trade_intensity"] = intensity;
    if (const auto* t = price_tick_at(in.ticks, ctx.canonical_pair_id, ctx.event_time_ms)) {
        ctx.price_tick = t->tick;
        ctx.instrument_spec_version = t->version;
    }
    // The archive keeps no projected zones. A structure target recorded on the
    // proposal IS the zone (BALANCED / AGGRESSIVE aim at its centre,
    // CONSERVATIVE at 0.85 of the way); any other basis means no zone
    // qualified, and a version-2 stop is never narrower than the version-1
    // one, so none would qualify now either.
    if (str(proposal, "target_basis") == "structure_zone") {
        const double e = num(proposal, "entry_price"), tp = num(proposal, "take_profit_price");
        ProjectedZone z;
        z.kind = side_of(str(proposal, "side")) == Side::long_ ? "resistance" : "support";
        z.center = i64(proposal, "plan_version") == 3 ? e + (tp - e) / 0.85 : tp;
        z.lower = z.upper = z.center;
        ctx.bitget_projected_zones.push_back(z);
    }
    c.candidate_id = candidate_id;
    c.canonical_pair_id = ctx.canonical_pair_id;
    c.context_id = ctx.context_id;
    c.strategy_id = str(cr, "strategy_id");
    c.strategy_version = str(cr, "strategy_version");
    c.strategy_family = family_from_string(str(cr, "strategy_family"));
    c.side = side_of(str(cr, "side"));
    c.holding_class = holding_from_string(str(cr, "holding_class"));
    c.expected_horizon_ms = i64(cr, "expected_horizon_ms");
    c.entry_hint = num(cr, "entry_hint", 0);
    c.invalidation_hint = num(cr, "invalidation_hint", 0);
    c.target_hint = num(cr, "target_hint", 0);
    c.raw_setup_score = num(cr, "raw_setup_score", 0);
    c.econ.horizon_minutes = static_cast<int32_t>(i64(cr, "econ_horizon_minutes"));
    if (const auto pi = in.predictions.find(candidate_id); pi != in.predictions.end()) c.predictions = pi->second;
    return true;
}

OutcomeGeometry geometry_of(const Row& p) {
    OutcomeGeometry g;
    g.side = side_of(str(p, "side"));
    g.entry_type = entry_of(str(p, "entry_type"));
    g.entry_price = num(p, "entry_price", 0);
    g.trigger_price = num(p, "trigger_price", 0);
    g.stop_loss_price = num(p, "stop_loss_price", 0);
    g.take_profit_price = num(p, "take_profit_price", 0);
    g.created_at_ms = i64(p, "created_at_ms");
    g.entry_fill_timeout_ms = i64(p, "entry_fill_timeout_ms");
    g.valid_until_ms = i64(p, "valid_until_ms");
    g.time_stop_at_ms = i64(p, "time_stop_at_ms");
    g.cost_bps = num(p, "round_trip_cost_bps", kLegacyCostModelV1Bps);
    return g;
}

OutcomeGeometry geometry_of(const TradePlanVariant& v, int64_t created_ms, double cost_bps) {
    OutcomeGeometry g;
    g.side = v.side;
    g.entry_type = v.entry_type;
    g.entry_price = v.entry_price;
    g.trigger_price = v.trigger_price;
    g.stop_loss_price = v.stop_loss_price;
    g.take_profit_price = v.take_profit_price;
    g.created_at_ms = created_ms;
    g.entry_fill_timeout_ms = v.entry_fill_timeout_ms;
    g.time_stop_at_ms = v.time_stop_at_ms;
    g.cost_bps = cost_bps;
    return g;
}

std::string fmt(double x) {
    if (!std::isfinite(x)) return "";
    char b[32];
    std::snprintf(b, sizeof(b), "%.6g", x);
    return b;
}

// One outcome as CSV columns; returns are blank where there is no position.
std::string cols(const MarketOutcome& o) {
    const bool traded = o.outcome == MarketOutcomeType::target_first || o.outcome == MarketOutcomeType::stop_first ||
                        o.outcome == MarketOutcomeType::time_stop;
    return to_string(o.outcome) + "," + o.resolution + "," + (traded ? fmt(o.realized_gross_return * 1e4) : "") +
           "," + (traded ? fmt(o.realized_net_return * 1e4) : "") + "," + std::to_string(o.holding_time_ms) + "," +
           fmt(o.mfe * 1e4) + "," + fmt(o.mae * 1e4);
}

} // namespace

int main(int argc, char** argv) {
    std::string dir, out_path;
    double horizons = 0;   // 0: the live rule (holding class cap)
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) dir = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--g2-time-stop-horizons" && i + 1 < argc) horizons = std::stod(argv[++i]);
    }
    if (dir.empty() || out_path.empty()) {
        std::cerr << "usage: altstrategy-outcome-replay --dir EXPORT_DIR --out FILE.csv\n";
        return 2;
    }
    try {
        Inputs in;
        read_csv(dir + "/d5_candles.csv", [&](const Row& r) {
            PathBar b;
            b.open_ms = i64(r, "open_ms");
            b.open = num(r, "open"); b.high = num(r, "high"); b.low = num(r, "low"); b.close = num(r, "close");
            in.candles.by_pair[i64(r, "instrument_id")][b.open_ms] = b;
        });
        read_csv(dir + "/d6_specs.csv", [&](const Row& r) {
            in.ticks[i64(r, "canonical_pair_id")].push_back(
                PriceTickVersion{i64(r, "valid_from_ms"), i64(r, "version"), num(r, "tick_size", 0)});
        });
        read_csv(dir + "/d2_candidates.csv", [&](const Row& r) { in.candidates[str(r, "candidate_id")] = r; });
        read_csv(dir + "/d4_contexts.csv", [&](const Row& r) { in.contexts[str(r, "context_id")] = r; });
        read_csv(dir + "/d3_candidate_predictions.csv",
                 [&](const Row& r) { in.predictions[str(r, "candidate_id")].push_back(prediction_of(r)); });

        std::ofstream out(out_path);
        if (!out) throw std::runtime_error("cannot write " + out_path);
        const std::string oc = "outcome,resolution,gross_bps,net_bps,hold_ms,mfe_bps,mae_bps";
        const auto pre = [&](const std::string& p) {
            std::string s;
            std::stringstream ss(oc);
            std::string c;
            while (std::getline(ss, c, ',')) s += "," + p + "_" + c;
            return s;
        };
        out << "signal_proposal_id,candidate_id,canonical_pair_id,strategy_id,side,plan_version,proposal_grade,"
               "created_at_ms,recorded_target_basis,recorded_target_bps,recorded_stop_bps,cost_bps"
            << pre("v1c") << pre("v2c")
            << ",g2_rebuilt,g2_entry_type,g2_entry,g2_stop,g2_target,g2_target_basis,g2_stop_basis,g2_target_bps,"
               "g2_stop_bps,g2_rr,g2_tick_source"
            << pre("g2") << ",g2_cost_bps\n";
        int64_t rows = 0, rebuilt = 0;
        read_csv(dir + "/d1_proposals_outcomes.csv", [&](const Row& p) {
            if (str(p, "proposal_grade") == "INVALID") return;   // never published, never matured
            const int64_t pair = i64(p, "canonical_pair_id");
            const auto g = geometry_of(p);
            const auto path = in.candles.path(pair, g.created_at_ms, g.time_stop_at_ms + kMin);
            const std::string sid = str(p, "signal_proposal_id"), cid = str(p, "candidate_id");
            const auto v1 = reconstruct_outcome(g, path.bars, OutcomePath::m1_candles, cid, sid, 0);
            const auto v2 = reconstruct_outcome_v2(g, path, cid, sid, 0);
            out << sid << "," << cid << "," << pair << "," << str(p, "strategy_id") << "," << str(p, "side") << ","
                << str(p, "plan_version") << "," << str(p, "proposal_grade") << "," << g.created_at_ms << ","
                << str(p, "target_basis") << "," << str(p, "target_distance_bps") << ","
                << str(p, "stop_distance_bps") << "," << fmt(g.cost_bps) << "," << cols(v1) << "," << cols(v2);
            StrategyCandidate c;
            DecisionContext ctx;
            if (rebuild(in, cid, c, ctx, p)) {
                const auto plans = generate_plans(c, ctx, g.created_at_ms);
                const int pv = static_cast<int>(i64(p, "plan_version", 1));
                const PlanProfile want = pv == 2 ? PlanProfile::aggressive : pv == 3 ? PlanProfile::conservative
                                                                                      : PlanProfile::balanced;
                TradePlanVariant* v = nullptr;
                auto variants = plans;
                for (auto& x : variants) if (x.profile == want) v = &x;
                if (v != nullptr && horizons > 0 && c.expected_horizon_ms > 0) {
                    const auto cap = static_cast<int64_t>(horizons * static_cast<double>(c.expected_horizon_ms));
                    if (cap < v->max_holding_time_ms) {
                        v->max_holding_time_ms = cap;
                        v->time_stop_at_ms = g.created_at_ms + cap;
                        v->entry_fill_timeout_ms = std::min(v->entry_fill_timeout_ms, cap);
                    }
                }
                if (v != nullptr) {
                    ++rebuilt;
                    // Priced exactly as the live proposal would be: the cost
                    // model over the rebuilt T0 market state and the plan's
                    // own entry leg.
                    const double cost = compute_trade_cost(v->entry_type, ctx).round_trip_bps;
                    const auto gp = in.candles.path(pair, g.created_at_ms, v->time_stop_at_ms + kMin);
                    const auto o2 = reconstruct_outcome_v2(geometry_of(*v, g.created_at_ms, cost), gp, cid, sid, 0);
                    out << ",1," << to_string(v->entry_type) << "," << fmt(v->entry_price) << ","
                        << fmt(v->stop_loss_price) << "," << fmt(v->take_profit_price) << "," << v->target_basis
                        << "," << v->stop_basis << "," << fmt(v->target_distance_bps) << ","
                        << fmt(v->stop_distance_bps) << "," << fmt(v->expected_rr) << "," << v->tick_source << ","
                        << cols(o2) << "," << fmt(cost) << "\n";
                    ++rows;
                    return;
                }
            }
            // g2_rebuilt = 0 and every other g2 column empty.
            out << ",0" << std::string(kG2Columns - 1, ',') << "\n";
            ++rows;
        });
        std::cerr << "rows=" << rows << " rebuilt=" << rebuilt << "\n";
    } catch (const std::exception& e) {
        std::cerr << "outcome-replay failed: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
