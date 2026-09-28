#pragma once

#include "altml/components/component.hpp"
#include "altml/core/config.hpp"

#include <memory>

namespace altml {

// Enforces the database size budget (`storage:` config section) and the
// per-table retention policy recorded in altml.retention_policies: routine
// partition drops once a table ages past its target, plus an emergency pass
// down to the floor (`minimum_keep`) if the database is still over its hard
// limit afterwards. Also caps the number of retired/rejected model rows and
// their on-disk artifacts kept, since those are not time-partitioned.
class StorageGuardComponent final : public Component {
public:
    explicit StorageGuardComponent(const config::RuntimeConfig& rt);
    ~StorageGuardComponent() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "storage-guard"; }
    [[nodiscard]] Result<void> init() override;
    void run() override;

private:
    struct Impl;
    const config::RuntimeConfig& rt_;
    std::unique_ptr<Impl> impl_;
};

} // namespace altml
