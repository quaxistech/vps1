#pragma once

// altstrategy.v1 decision-engine NATS subjects (spec §46 inputs, §47 outputs).
//
// AltStrategy assembles its own DecisionContext from the AltTech analytics
// component subjects + the AltSpotter universe + the AltML forecast, requests
// candidate evaluations from AltML, and OWNS the decision output stream. It
// never publishes order commands.
//
// Input subject *roots* are defaults; the real values are configurable in
// altstrategy.yaml (inputs:) so the wiring can follow the upstream services.

#include <string>
#include <string_view>

namespace altstrategy::nats_subjects {

// ------------------------------ inputs (§46) --------------------------------
// AltSpotter global universe.
// INBOUND subjects are not declared here. The engine subscribes to exactly the
// patterns in `inputs.alttech_subjects`, `inputs.prediction_subjects` and
// `inputs.universe_subjects`, which are validated at startup. This header used
// to also carry eleven inbound constants that nothing read -- a second,
// silently divergent declaration of the same namespace.

// ------------------------------ outputs (§47) -------------------------------
// AltStrategy owns everything below (JetStream ALTSTRATEGY_DECISIONS).

inline std::string candidate(std::string_view symbol, std::string_view strategy) {
    return std::string("strategy.candidate.").append(symbol).append(".").append(strategy).append(".v1");
}
inline std::string ensemble(std::string_view symbol) {
    return std::string("strategy.ensemble.").append(symbol).append(".v1");
}
inline std::string ranking(std::string_view symbol) {
    return std::string("strategy.ranking.").append(symbol).append(".v1");
}
inline std::string trade_proposal(std::string_view symbol) {
    return std::string("tradeproposal.").append(symbol).append(".v1");
}
inline std::string signal_proposed(std::string_view symbol) {
    return std::string("signal.proposed.").append(symbol).append(".v1");
}
inline std::string market_outcome(std::string_view symbol) {
    return std::string("signal.market_outcome.").append(symbol).append(".v1");
}

// `strategy.lifecycle.*`, `strategy.drift.*` and `strategy.calibration.*` are
// NOT built here. Drift snapshots, lifecycle transitions and parameter
// calibrations are computed and written to PostgreSQL
// (`strategy_drift_snapshots`, `lifecycle_events`, `parameter_calibration_runs`);
// nothing publishes them, and a builder for a subject nothing sends is an
// invitation to document a contract that does not exist.

} // namespace altstrategy::nats_subjects

