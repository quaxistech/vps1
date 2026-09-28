#pragma once

#include "altsignalquality/components/component.hpp"
#include "altsignalquality/core/config.hpp"

#include <memory>
#include <string>
#include <vector>

namespace altsignalquality {

// The native in-process trainer: no Python, no external job, no timer.
//
// On first start there is no model, and the service serves under its cold-start
// policy while the dataset accumulates. Once `training.min_rows` eligible
// examples exist the trainer runs a cycle on its own, and afterwards it
// re-trains on a rolling window at the configured cadence.
//
// A cycle only ever ADDS a CANDIDATE model. Promotion to ACTIVE requires the
// full gate set (docs/PROMOTION_GATES.md): walk-forward stability, a filter
// that measurably beats "take every AltStrategy signal" on the held-out slice,
// a calibrated score, and — when there is an incumbent — beating it on the very
// same holdout. A model that fails any gate is registered REJECTED with the
// reasons, so a bad candidate is a record, not a silent non-event.
class TrainerComponent final : public Component {
public:
    explicit TrainerComponent(const config::RuntimeConfig& rt);
    ~TrainerComponent() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "trainer"; }
    [[nodiscard]] Result<void> init() override;
    void run() override;

private:
    struct Impl;
    const config::RuntimeConfig& rt_;
    std::unique_ptr<Impl> impl_;
};

// One synchronous training cycle for `altsignalquality train` (diagnostics /
// controlled re-run / CI). `force` skips only the schedule and resource-pressure
// checks; every safety, leakage and promotion gate still applies.
// Exit codes: 0 trained, 1 cycle failed, 2 config, 3 deferred under pressure.
// One §57 ablation arm: the production training cycle with `groups` blanked.
struct AblationArmResult {
    int64_t rows = 0;
    std::string status;
    double roc_auc = 0, pr_auc = 0, brier = 0;
    double expectancy_bps = 0;         // mean net return per ACCEPTED signal
    double expectancy_uplift_bps = 0;  // against the unfiltered baseline (§56)
};

namespace trainer {
// `applicable_all` measures on the WHOLE signal population rather than the
// upstream-recommended subset. It is the default since the recommended-only
// policy was retired (VPS1 recommends nothing for execution); the flag is kept
// so an old ablation can be re-run as it was. An arm says which in its status.
Result<AblationArmResult> run_ablation_arm(const config::RuntimeConfig& rt,
                                           const std::vector<std::string>& groups,
                                           bool applicable_all = false);
}

int run_training_once(const config::RuntimeConfig& rt, bool force);

// Validate the current dataset and the ACTIVE model without training: the
// leakage audit, the feature-contract/schema match, artifact integrity and the
// recorded sample-prediction parity. The audit gate behind
// `altsignalquality validate`.
int run_validation(const config::RuntimeConfig& rt);

} // namespace altsignalquality
