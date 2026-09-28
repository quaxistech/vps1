#pragma once

#include "altml/components/component.hpp"
#include "altml/core/config.hpp"
#include "altml/core/nats_client.hpp"

#include <memory>

namespace altml {

// Native CPU inference over live core feature vectors.
//
//   input : durable pull consumer (project.input.durable) on the project's
//           input stream (alttech.v1 feature vectors; re-packed to altml.v1 on
//           ingest);
//   model : the single ACTIVE lightgbm model from the registry (polled every
//           30s; checksum verified on load; feature compatibility enforced);
//   output: <prediction_subject_prefix>.<sym>.1m.<model_version>.
//
// Modes: with no ACTIVE model the component idles healthily — the
// deterministic pipeline (DETERMINISTIC_ONLY) never depends on it. Stale
// vectors (older than stale_prediction_sec) are dropped, never predicted.
// No training here.
class InferenceComponent final : public Component {
public:
    InferenceComponent(const config::RuntimeConfig& rt, messaging::NatsClient& nats);
    ~InferenceComponent() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "inference"; }
    [[nodiscard]] Result<void> init() override;
    void run() override;

private:
    struct Impl;
    const config::RuntimeConfig& rt_;
    messaging::NatsClient& nats_;
    std::unique_ptr<Impl> impl_;
};

} // namespace altml
