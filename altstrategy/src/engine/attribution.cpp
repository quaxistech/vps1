#include "altstrategy/engine/attribution.hpp"

#include "altstrategy/engine/ids.hpp"

#include <algorithm>
#include <cmath>

namespace altstrategy::engine {

OutcomeAttribution attribute_outcome(const TradeSignalProposal& p, const MarketOutcome& m,
                                     const std::optional<ExecutionFeedback>& exec,
                                     double ranking_regret, bool regime_changed, int64_t now_ms) {
    OutcomeAttribution a;
    a.signal_proposal_id = p.signal_proposal_id;
    a.attribution_id = ids::make("attr", p.signal_proposal_id);
    a.created_at_ms = now_ms;

    // ---------------------------------------------------------------------
    // The decomposition is an IDENTITY, not a set of scores:
    //
    //     realized_gross = ml + strategy + geometry
    //     realized_net   = realized_gross + cost
    //
    // Every term is measured; none is a tuned fraction. `ranking` and
    // `execution` sit OUTSIDE the identity on purpose and are documented below.
    // ---------------------------------------------------------------------
    const double realized_gross = m.realized_gross_return;   // market-reconstructed

    // Cost is read from the versioned figure the plan was priced with, not
    // reverse-engineered from a gross-minus-net gap. That subtraction assumed
    // both terms were forecasts; when there was no model both were fabricated,
    // so the "cost" it recovered was an artefact of the fabrication.
    const double cost = p.round_trip_cost_bps / 10'000.0;
    a.cost_attribution = -std::abs(cost);

    // ML share: the part of the realized move the forecast actually anticipated.
    // Sign-aligned and capped at the realized move, so a forecast can never be
    // credited with more than happened, nor credited at all when it pointed the
    // wrong way.
    // ONLY a real forecast may be attributed to the model. Without one this is
    // zero, so the model is neither credited nor blamed. Previously this read
    // expected_net_return unconditionally -- which, with no active model, was
    // -cost: a negative number on every trade, charging the ML term for losses
    // it had not predicted and could not have.
    const double ml_pred = p.edge_estimated ? p.expected_net_return : 0.0;
    a.ml_attribution = (ml_pred != 0 && std::signbit(ml_pred) == std::signbit(realized_gross))
                           ? std::copysign(std::min(std::abs(ml_pred), std::abs(realized_gross)), realized_gross)
                           : 0.0;

    // Path bias — what the price path offered after entry, independent of where
    // the exits sat. MFE and MAE are direction-normalised non-negative
    // excursions, so their difference is the signed directional edge the
    // strategy's entry actually produced: positive when price ran the trade's
    // way more than against it. This is the term that answers "was the call
    // right?", and it is charged negative when the entry was simply wrong.
    const double path_bias = m.mfe - m.mae;

    // Strategy share: the directional edge the entry produced, less the part the
    // forecast had already claimed.
    a.strategy_attribution = path_bias - a.ml_attribution;

    // Geometry share: the residual — how stop/target/time-stop placement
    // converted that path bias into the realized result. Exiting at the peak
    // earns credit; giving back a favourable excursion is charged here. Defined
    // as the residual so the three shares sum to realized_gross exactly.
    a.geometry_attribution = realized_gross - path_bias;

    // Regime share: when the regime changed mid-trade the strategy was running
    // outside the conditions it was selected under, so its directional credit is
    // REALLOCATED to regime rather than scaled by an invented fraction. The
    // identity is preserved because the amount is moved, not created.
    if (regime_changed) {
        a.regime_attribution = a.strategy_attribution;
        a.strategy_attribution = 0.0;
    } else {
        a.regime_attribution = 0.0;
    }

    // Ranking opportunity cost sits OUTSIDE the identity: it is counterfactual
    // (what a candidate we did not take would have returned), so it is never a
    // component of a return we actually realized.
    a.ranking_attribution = -std::max(0.0, ranking_regret);

    // Execution share also sits outside the identity: `realized_gross` is
    // reconstructed from the market path, so real fill slippage is an ADDITIONAL
    // divergence from that reconstruction. Isolating it is the whole point — a
    // pure execution problem must never be charged to the strategy (§22).
    a.execution_attribution = (exec && exec->position_closed)
                                  ? -std::abs(exec->slippage_bps) / 10000.0
                                  : 0.0;

    // Dominant factor by magnitude.
    struct C { const char* name; double v; };
    const C comps[] = {{"strategy", a.strategy_attribution}, {"ranking", a.ranking_attribution},
                       {"geometry", a.geometry_attribution}, {"ml", a.ml_attribution},
                       {"regime", a.regime_attribution},     {"cost", a.cost_attribution},
                       {"execution", a.execution_attribution}};
    double best = -1; const char* dom = "strategy";
    for (const auto& c : comps) if (std::abs(c.v) > best) { best = std::abs(c.v); dom = c.name; }
    a.dominant_factor = dom;
    return a;
}

} // namespace altstrategy::engine
