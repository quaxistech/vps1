#include "altstrategy/engine/decision_types.hpp"

#include <string>

namespace altstrategy::engine {

MlEvaluation ml_from_forecast(const MarketForecastView& f) {
    // Derived from the prediction already in the context, not from a request.
    //
    // This used to publish on request.candidate_evaluation.* and block on a
    // reply. Nothing in VPS1 answers that subject -- not AltML, not AltTech,
    // not AltSignalQuality -- so the wait always timed out and every proposal
    // was scored ML_UNAVAILABLE while a real PredictionV1 sat unread in the
    // context. AltML publishes; AltStrategy consumes. There is no question to
    // ask, so there is nothing to wait for.
    MlEvaluation out;
    if (!f.present) {
        out.unavailable = true;   // no prediction: NOT p = 0.5
        return out;
    }
    out.present = true;
    out.abstain = f.abstain;
    if (f.abstain) return out;    // probabilities are meaningless when abstaining

    out.p_up = f.p_up;
    out.p_down = f.p_down;
    out.p_target_before_stop = f.probability_target_before_stop;
    out.expected_net_return = f.expected_return;
    out.expected_mfe = f.expected_mfe;
    out.expected_mae = f.expected_mae;
    out.expected_holding_time_min = f.expected_holding_time_min;
    out.conviction = f.conviction;
    out.quality_status = f.quality_status;
    if (!f.model_id.empty()) {
        out.model_versions.push_back(f.model_id + ":" + std::to_string(f.model_version));
    }
    return out;
}

} // namespace altstrategy::engine
