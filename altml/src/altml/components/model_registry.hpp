#pragma once

#include "altml/components/component.hpp"
#include "altml/core/config.hpp"
#include "altml/core/nats_client.hpp"
#include "altml/core/pg.hpp"

#include <map>
#include <optional>
#include <string>

namespace altml {

// Audit/event bridge over the ML model registry.
//
// Polls altml.models for status transitions and publishes them as
// evt.model.<event> on the project's control stream so operators and
// consuming applications observe lifecycle changes without watching the
// database. The registry tables are the source of truth (written by
// training/promote tools); this component never mutates model state.
class ModelRegistryComponent final : public Component {
public:
    ModelRegistryComponent(const config::RuntimeConfig& rt, messaging::NatsClient& nats);

    [[nodiscard]] std::string_view name() const noexcept override { return "model-registry"; }
    [[nodiscard]] Result<void> init() override;
    void run() override;

private:
    const config::RuntimeConfig& rt_;
    messaging::NatsClient& nats_;
    int64_t poll_sec_ = 30;
    std::optional<persistence::PgPool> pool_;
    std::map<std::string, std::string> known_; // model_id -> status
    bool first_pass_ = true;
};

} // namespace altml
