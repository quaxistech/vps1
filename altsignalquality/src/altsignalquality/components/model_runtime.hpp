#pragma once

// The live model: loading a registered LightGBM artifact and scoring one signal.
//
// Loading is defensive on purpose, because every one of these checks
// corresponds to a way a model can be wrong while looking fine:
//
//   * the registry path is resolved and confined to the configured model root —
//     a registry row is data, not authority;
//   * the artifact's sha256 must equal the checksum recorded at registration —
//     a truncated or edited model file is refused, not scored with;
//   * the booster's own feature count must equal the registry's, and the
//     registry's schema hash must equal the RUNNING contract's — a model
//     trained on a different feature list would read every position as
//     something else and produce confident nonsense;
//   * a model that fails any of these leaves the service in MODEL_UNUSABLE,
//     which the decision policy handles explicitly. It never falls back to
//     scoring with a stale or mismatched booster.
//
// Inference is deterministic: LightGBM's `PredictForMat` on one row, single
// thread, no random state, followed by the model's own isotonic calibration.

#include "altsignalquality/core/error.hpp"
#include "altsignalquality/data/store.hpp"
#include "altsignalquality/domain/calibration.hpp"
#include "altsignalquality/domain/feature_contract.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace altsignalquality {

struct ScoredSignal {
    double raw_score = 0;
    double quality_score = 0;   // after calibration
    int64_t latency_us = 0;
};

// A loaded, verified model. Immutable once constructed.
class LoadedModel {
public:
    ~LoadedModel();
    LoadedModel(const LoadedModel&) = delete;
    LoadedModel& operator=(const LoadedModel&) = delete;

    // Verify and load. `model_root` confines the artifact path.
    [[nodiscard]] static Result<std::unique_ptr<LoadedModel>> open(
        const data::ActiveModel& registry_row, const std::filesystem::path& model_root);

    [[nodiscard]] Result<ScoredSignal> score(const domain::FeatureVector& features) const;

    [[nodiscard]] const std::string& model_id() const { return meta_.model_id; }
    [[nodiscard]] const data::ActiveModel& meta() const { return meta_; }
    [[nodiscard]] const domain::Calibration& calibration() const { return calibration_; }

private:
    LoadedModel() = default;

    data::ActiveModel meta_;
    domain::Calibration calibration_;
    void* booster_ = nullptr;   // BoosterHandle; opaque so LightGBM stays out of headers
};

// Holds the current model and swaps it when the registry's ACTIVE row changes.
// Reads are lock-free-ish (one mutex, held only for the pointer copy), so the
// hot path never blocks behind a reload.
class ModelHolder {
public:
    ModelHolder(data::Store& store, std::string objective, std::filesystem::path model_root)
        : store_(store), objective_(std::move(objective)), model_root_(std::move(model_root)) {}

    // Poll the registry; load, swap or drop as needed. Never throws; a failure
    // leaves the previous state untouched and reports it.
    void refresh();

    [[nodiscard]] std::shared_ptr<const LoadedModel> current() const {
        std::lock_guard lock(mu_);
        return model_;
    }
    // True when the registry has an ACTIVE model that this process could not
    // use — as opposed to there being no model at all.
    [[nodiscard]] bool unusable() const {
        std::lock_guard lock(mu_);
        return unusable_;
    }

private:
    data::Store& store_;
    std::string objective_;
    std::filesystem::path model_root_;
    mutable std::mutex mu_;
    std::shared_ptr<const LoadedModel> model_;
    bool unusable_ = false;
    std::string last_error_;
};

// sha256 of a file's contents, or empty on read failure.
[[nodiscard]] std::string file_sha256(const std::string& path);

} // namespace altsignalquality
