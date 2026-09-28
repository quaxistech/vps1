#include "altsignalquality/components/model_runtime.hpp"

#include <LightGBM/c_api.h>

#include "altsignalquality/core/hash.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/time.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

namespace altsignalquality {

namespace {

namespace fs = std::filesystem;

// True when `child` is strictly inside `root` after both are canonicalised.
bool is_within(const fs::path& root, const fs::path& child) {
    auto root_it = root.begin();
    auto child_it = child.begin();
    for (; root_it != root.end(); ++root_it, ++child_it) {
        if (child_it == child.end() || *root_it != *child_it) return false;
    }
    return child_it != child.end();
}

} // namespace

std::string file_sha256(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return sha256_hex(ss.str());
}

LoadedModel::~LoadedModel() {
    if (booster_ != nullptr) LGBM_BoosterFree(static_cast<BoosterHandle>(booster_));
}

Result<std::unique_ptr<LoadedModel>> LoadedModel::open(const data::ActiveModel& row,
                                                       const fs::path& model_root) {
    // 1. The path in the registry is data. Resolve it and confine it to the
    //    configured root before opening anything.
    std::error_code root_ec, artifact_ec;
    const auto canonical_root = fs::weakly_canonical(model_root, root_ec);
    const auto canonical_artifact = fs::canonical(row.artifact_path, artifact_ec);
    if (root_ec || artifact_ec || !is_within(canonical_root, canonical_artifact)) {
        return fail(ErrorCode::model_error,
                    "model artifact path is outside the configured model root: " +
                        row.artifact_path);
    }
    const std::string path = canonical_artifact.string();

    // 2. The bytes must be the bytes that were registered.
    const std::string actual = file_sha256(path);
    if (actual.empty()) {
        return fail(ErrorCode::model_error, "model artifact is unreadable: " + path);
    }
    if (actual != row.artifact_checksum) {
        return fail(ErrorCode::model_error,
                    "model artifact checksum mismatch for " + row.model_id +
                        " (registered " + row.artifact_checksum + ", on disk " + actual + ")");
    }

    // 3. The contract the model was trained under must be the contract running
    //    now. A positional vector against the wrong list is silent nonsense.
    if (row.feature_schema_version != std::string(domain::kFeatureSchemaVersion) ||
        row.feature_schema_hash != domain::feature_schema_hash()) {
        return fail(ErrorCode::model_error,
                    "model " + row.model_id + " was trained under feature schema " +
                        row.feature_schema_version + " (" + row.feature_schema_hash.substr(0, 12) +
                        "), this binary runs " + std::string(domain::kFeatureSchemaVersion) +
                        " (" + domain::feature_schema_hash().substr(0, 12) + ")");
    }

    auto model = std::unique_ptr<LoadedModel>(new LoadedModel());
    model->meta_ = row;
    BoosterHandle booster = nullptr;
    int iterations = 0;
    if (LGBM_BoosterCreateFromModelfile(path.c_str(), &iterations, &booster) != 0) {
        return fail(ErrorCode::model_error,
                    std::string("LightGBM could not load ") + path + ": " + LGBM_GetLastError());
    }
    model->booster_ = booster;

    int n_features = 0;
    LGBM_BoosterGetNumFeature(booster, &n_features);
    if (n_features != static_cast<int>(domain::kFeatureCount) ||
        n_features != row.feature_count) {
        return fail(ErrorCode::model_error,
                    "model " + row.model_id + " expects " + std::to_string(n_features) +
                        " features, registry says " + std::to_string(row.feature_count) +
                        ", contract declares " + std::to_string(domain::kFeatureCount));
    }

    model->calibration_.method = row.calibration_method;
    model->calibration_.version = row.calibration_version;
    model->calibration_.x = row.calibration_x;
    model->calibration_.y = row.calibration_y;
    if (row.calibration_method == "isotonic" && !model->calibration_.usable()) {
        return fail(ErrorCode::model_error,
                    "model " + row.model_id + " has an unusable isotonic calibration");
    }
    return model;
}

Result<ScoredSignal> LoadedModel::score(const domain::FeatureVector& features) const {
    if (features.values.size() != domain::kFeatureCount) {
        return fail(ErrorCode::validation_error,
                    "feature vector has " + std::to_string(features.values.size()) +
                        " values, contract declares " + std::to_string(domain::kFeatureCount));
    }
    const auto started = std::chrono::steady_clock::now();
    double raw = 0;
    int64_t out_len = 0;
    if (LGBM_BoosterPredictForMat(static_cast<BoosterHandle>(booster_), features.values.data(),
                                  C_API_DTYPE_FLOAT32, 1,
                                  static_cast<int32_t>(domain::kFeatureCount), 1,
                                  C_API_PREDICT_NORMAL, 0, -1, "num_threads=1", &out_len,
                                  &raw) != 0 ||
        out_len != 1) {
        return fail(ErrorCode::model_error,
                    std::string("LightGBM prediction failed: ") + LGBM_GetLastError());
    }
    ScoredSignal out;
    out.raw_score = raw;
    out.quality_score = calibration_.apply(raw);
    out.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - started)
                         .count();
    return out;
}

void ModelHolder::refresh() {
    auto row = store_.load_active_model(objective_);
    if (!row) {
        // A database blip must not silently disarm the filter: keep serving the
        // model already in memory and say so.
        log::warn("model_poll_failed", {{"message", row.error().message}});
        return;
    }
    if (!row->has_value()) {
        std::lock_guard lock(mu_);
        if (model_) {
            log::warn("model_deactivated", {{"model_id", model_->model_id()}});
            model_.reset();
        }
        unusable_ = false;
        return;
    }
    {
        std::lock_guard lock(mu_);
        if (model_ && model_->model_id() == (*row)->model_id) return;  // unchanged
    }
    auto loaded = LoadedModel::open(**row, model_root_);
    if (!loaded) {
        std::lock_guard lock(mu_);
        // Drop the previous model too: the registry says THIS one is
        // production, and continuing to score with a retired model would make
        // the published model_id a lie.
        model_.reset();
        unusable_ = true;
        if (last_error_ != loaded.error().message) {
            last_error_ = loaded.error().message;
            log::error("model_load_failed",
                       {{"model_id", (*row)->model_id}, {"message", last_error_}});
        }
        return;
    }
    std::shared_ptr<const LoadedModel> shared = std::move(*loaded);
    {
        std::lock_guard lock(mu_);
        model_ = std::move(shared);
        unusable_ = false;
        last_error_.clear();
    }
    log::info("model_activated_in_process",
              {{"model_id", (*row)->model_id},
               {"features", (*row)->feature_count},
               {"decision_threshold", (*row)->decision_threshold},
               {"calibration", (*row)->calibration_method},
               {"calibration_version", (*row)->calibration_version}});
}

} // namespace altsignalquality
