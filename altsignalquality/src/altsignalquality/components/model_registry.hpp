#pragma once

#include "altsignalquality/components/component.hpp"
#include "altsignalquality/core/config.hpp"
#include "altsignalquality/core/nats_client.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>

namespace altsignalquality {

namespace persistence { class PgPool; }

// Audit bridge over the model registry.
//
// Polls altsignalquality.models for status transitions and republishes them on
// the service's own control subjects, so an operator or a downstream consumer
// sees a promotion without watching the database. The registry is the source of
// truth; this component only observes it and never mutates model state.
class ModelRegistryComponent final : public Component {
public:
    ModelRegistryComponent(const config::RuntimeConfig& rt, messaging::NatsClient& nats);
    ~ModelRegistryComponent() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "model-registry"; }
    [[nodiscard]] Result<void> init() override;
    void run() override;

private:
    struct Impl;
    const config::RuntimeConfig& rt_;
    messaging::NatsClient& nats_;
    std::unique_ptr<Impl> impl_;
};

} // namespace altsignalquality
