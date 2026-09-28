#pragma once

#include "altsignalquality/components/component.hpp"
#include "altsignalquality/core/config.hpp"
#include "altsignalquality/core/nats_client.hpp"

#include <memory>

namespace altsignalquality {

// The live filter.
//
//   input  : a durable pull consumer on AltStrategy's decision stream, filtered
//            to the candidate proposals (signal.proposed.>). Durable, so a
//            restart resumes at the cursor and nothing is silently dropped.
//   work   : rebuild the point-in-time snapshot, assemble the contract feature
//            vector, score it with the ACTIVE model, apply the policy.
//   output : the FINAL TradeSignal for accepted candidates, and a quality
//            evaluation event for every candidate either way.
//
// Ordering is chosen for idempotency, not convenience: the evaluation row is
// CLAIMED in PostgreSQL before anything is published, and only marked published
// afterwards. A crash in between leaves an accepted-but-unpublished row, which
// startup replays under the same deterministic final_signal_id — so the signal
// is never lost, and JetStream's de-duplication makes the re-publish harmless.
class EvaluatorComponent final : public Component {
public:
    EvaluatorComponent(const config::RuntimeConfig& rt, messaging::NatsClient& nats);
    ~EvaluatorComponent() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "evaluator"; }
    [[nodiscard]] Result<void> init() override;
    void run() override;

private:
    struct Impl;
    const config::RuntimeConfig& rt_;
    messaging::NatsClient& nats_;
    std::unique_ptr<Impl> impl_;
};

} // namespace altsignalquality
