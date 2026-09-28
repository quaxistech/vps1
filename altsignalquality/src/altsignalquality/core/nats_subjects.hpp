#pragma once

// AltSignalQuality NATS subjects.
//
// INPUT (externally owned, never created here): AltStrategy publishes the main
// plan variant of every non-INVALID proposal on `signal.proposed.<symbol>.v1`
// over core NATS; those subjects are captured by AltStrategy's own
// ALTSTRATEGY_DECISIONS JetStream stream (bound to `signal.>`), which is what
// gives this service durable, replayable, at-least-once delivery with a
// restart-safe cursor. AltSignalQuality owns only its durable consumer there.
//
// OUTPUT (owned): everything under `signalquality.>`. The FINAL TradeSignal is
// the only executable artefact; the evaluation stream carries ACCEPTED and
// REJECTED alike so the filter can be audited from the bus alone.

#include "altsignalquality/core/config.hpp"

#include <string>
#include <string_view>

namespace altsignalquality::nats_subjects {

// A symbol token that is empty or contains NATS wildcards/separators would
// produce an unroutable or ambiguous subject; fall back to the pair id.
[[nodiscard]] std::string subject_token(std::string_view symbol, int64_t canonical_pair_id);

[[nodiscard]] inline std::string final_signal(const config::WiringConfig& w,
                                              std::string_view token) {
    return w.final_subject_prefix + "." + std::string(token) + ".v1";
}

[[nodiscard]] inline std::string evaluation(const config::WiringConfig& w,
                                            std::string_view token) {
    return w.evaluation_subject_prefix + "." + std::string(token) + ".v1";
}

[[nodiscard]] inline std::string model_event(const config::WiringConfig& w,
                                             std::string_view event) {
    return w.model_event_subject_prefix + "." + std::string(event) + ".v1";
}

} // namespace altsignalquality::nats_subjects
