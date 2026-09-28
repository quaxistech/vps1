#pragma once

#include "altsignalquality/components/component.hpp"
#include "altsignalquality/core/config.hpp"

#include <memory>

namespace altsignalquality {

// The learning loop's data half.
//
// Runs three periodic jobs in one thread, all of them idempotent and all of
// them safe to interrupt at any point:
//
//   priors      — fold newly matured AltStrategy outcomes into the as-of
//                 aggregates the feature contract reads. Incremental against a
//                 durable watermark, so a restart resumes rather than replays.
//   examples    — turn matured signals into immutable training examples. LIVE
//                 rows reuse the exact vector this service scored at T0;
//                 BACKFILL rows reconstruct one through the SAME assembler from
//                 AltStrategy's point-in-time archive, with priors replayed
//                 as-of the signal rather than as-of now.
//   retention   — bound this schema's own tables. Never touches another
//                 service's data.
//
// It never recomputes an outcome: AltStrategy owns outcome reconstruction and
// this component only reads what it decided.
class DatasetComponent final : public Component {
public:
    explicit DatasetComponent(const config::RuntimeConfig& rt);
    ~DatasetComponent() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "dataset"; }
    [[nodiscard]] Result<void> init() override;
    void run() override;

private:
    struct Impl;
    const config::RuntimeConfig& rt_;
    std::unique_ptr<Impl> impl_;
};

// One synchronous dataset pass, for `altsignalquality dataset-build`
// (diagnostics / catching up after a long outage / CI). Returns a process exit
// code.
int run_dataset_once(const config::RuntimeConfig& rt);

} // namespace altsignalquality
