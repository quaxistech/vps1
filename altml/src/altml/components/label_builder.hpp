#pragma once

#include "altml/components/component.hpp"
#include "altml/core/config.hpp"

#include <memory>

namespace altml {

// Builds future-outcome labels for closed 1m bars (label set "core-labels").
// Pure database batch worker: no NATS. See label_builder.cpp for the label
// catalog and leakage discipline.
class LabelBuilderComponent final : public Component {
public:
    explicit LabelBuilderComponent(const config::RuntimeConfig& rt);
    ~LabelBuilderComponent() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "label-builder"; }
    [[nodiscard]] Result<void> init() override;
    void run() override;

private:
    struct Impl;
    const config::RuntimeConfig& rt_;
    std::unique_ptr<Impl> impl_;
};

} // namespace altml
