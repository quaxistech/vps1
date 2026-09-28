#include "altsignalquality/components/trainer.hpp"

#include <LightGBM/c_api.h>
#include <nlohmann/json.hpp>

#include "altsignalquality/components/model_runtime.hpp"
#include "altsignalquality/core/hash.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/pg.hpp"
#include "altsignalquality/core/readiness.hpp"
#include "altsignalquality/core/shutdown.hpp"
#include "altsignalquality/core/time.hpp"
#include "altsignalquality/data/store.hpp"
#include "altsignalquality/domain/calibration.hpp"
#include "altsignalquality/domain/feature_contract.hpp"
#include "altsignalquality/domain/feature_diagnostics.hpp"
#include "altsignalquality/domain/labeling.hpp"
#include "altsignalquality/domain/leakage.hpp"
#include "altsignalquality/domain/metrics.hpp"
#include "altsignalquality/domain/policy.hpp"
#include "altsignalquality/domain/significance.hpp"
#include "altsignalquality/domain/split.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>

namespace altsignalquality {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kTrainerVersion = "sq-trainer-1.0.0";
constexpr int kSampleRows = 64;
constexpr double kSelfCheckTolerance = 1e-9;

// ------------------------------------------------------------- configuration

struct TrainerConfig {
    // Off by default: this service decides validity, and its learned score
    // never gates a signal (policy.hpp, decide()). A model it cannot use for a
    // decision is trained only when an operator asks for the diagnostic.
    bool enabled = false;
    bool bootstrap_if_no_active = true;
    bool auto_promote = true;
    int64_t interval_hours = 24;
    int64_t start_hour_utc = -1;
    int64_t retry_minutes = 60;
    int64_t lookback_days = 180;
    int64_t max_rows = 400'000;
    int64_t min_rows = 400;
    int64_t folds = 3;
    // The embargo sits ON TOP of the maturation purge. A signal's own label can
    // arrive up to the 48h hard holding limit later, and adjacent signals on the
    // same instrument overlap; 12h of extra gap covers the residual correlation
    // without eating a short history.
    int64_t embargo_minutes = 720;
    double validation_fraction = 0.25;

    // LightGBM. Deliberately small and heavily regularised: this dataset is
    // hundreds-to-thousands of signals against 111 features, which is exactly
    // the regime where a deep forest memorises the training slice and the
    // walk-forward gates then (correctly) reject everything it produces.
    double learning_rate = 0.03;
    int64_t num_leaves = 15;
    int64_t min_data_in_leaf = 30;
    int64_t max_depth = 4;
    double feature_fraction = 0.5;
    double bagging_fraction = 0.8;
    int64_t bagging_freq = 1;
    double lambda_l1 = 0.5;
    double lambda_l2 = 10.0;
    int64_t max_iterations = 500;
    int64_t early_stopping_rounds = 40;
    int64_t min_iterations = 10;
    int64_t threads = 1;
    int64_t time_budget_sec = 900;
    int64_t max_matrix_mib = 256;

    // Threshold search.
    double min_acceptance_rate = 0.15;
    double max_acceptance_rate = 0.95;
    int64_t min_accepted = 30;

    // Calibration.
    int64_t min_calibration_rows = 100;
    // The threshold is chosen on the APPLICABLE population -- the signals policy
    // would actually forward. With `evaluate_on_recommended_only` that is the
    // upstream-recommended subset, which on this stack is a small fraction of
    // all archived signals. Below this many rows no threshold can be chosen
    // from evidence, and the correct outcome is "not enough data yet", not a
    // failed run.
    int64_t min_applicable_rows = 200;
    // Feature groups blanked before training, for §57 ablation. Empty = full
    // contract. A blanked feature is marked ABSENT, not set to zero: "we did
    // not have this input" is the counterfactual being measured, and a zero
    // would instead say "we had it and it read zero".
    std::vector<std::string> ablate_groups;

    // Promotion gates.
    double min_mean_fold_auc = 0.52;
    int64_t min_positive_folds = 2;
    double min_return_per_candidate_uplift = 0.0;
    double min_win_rate_uplift = 0.0;
    double max_ece = 0.15;
    double min_stability_share = 0.60;
    int64_t stability_min_bucket_samples = 25;
    // The uplift's lower confidence bound must clear this. A point estimate is
    // not a result: at this stack's dispersion a hundred trades routinely swing
    // tens of basis points with no edge behind them, so a model is promoted on
    // the bound, not on the mean.
    double min_uplift_lower_bound = 0.0;
    double bootstrap_confidence = 0.95;
    int64_t bootstrap_replicates = 2000;
    // A dataset that is nearly one-class cannot train a useful filter, and the
    // metrics on it are misleading rather than merely weak.
    double min_positive_rate = 0.05;
    double max_positive_rate = 0.95;
    // At least this share of the declared contract must actually vary in the
    // dataset. Below it the model is being fitted on a handful of columns while
    // the report claims a 110-feature contract.
    double min_informative_feature_share = 0.30;

    // Resource guard.
    //
    // CPU availability is read from PSI (`/proc/pressure/cpu`), not from the
    // load average. Linux counts tasks in uninterruptible I/O sleep towards the
    // load average, so on a host that also runs the database it tracks disk
    // pressure rather than CPU. Measured on VPS1: load average 9.9 on 8 cores
    // while 30% of CPU time was idle, PSI cpu `full avg60` 0.00 against io
    // `full avg60` 9.75. The gate was deferring a threads=1 job over a run
    // queue that was long for reasons it does not even measure -- 70 deferrals
    // in three days, with cycle gaps of up to 27h against retry_minutes=60.
    //
    // `full` is the share of the last minute during which NO task could run for
    // want of CPU. Sustained double-digit `full` pressure is real saturation;
    // this host's steady state is zero.
    double max_cpu_pressure_pct = 20.0;
    // Fallback for kernels without PSI, unchanged in meaning: an absolute
    // one-minute load average.
    double max_load_average = 6.0;
    int64_t min_free_mem_mib = 384;

    // Mirrors policy.require_upstream_recommendation. When the live filter only
    // ever sees candidates AltStrategy recommended, every threshold, gate and
    // stability check must be measured on THAT population — measuring on the
    // whole book would tune and certify the model against signals it will never
    // be shown. On this stack's history that is 1 189 of 18 659 labelled rows,
    // so the difference is not academic.
    // false since the recommendation-based policy was retired: the live
    // filter forwards every valid signal, so the (disabled) trainer measures
    // on the whole population it would be shown.
    bool evaluate_on_recommended_only = false;
};

Result<TrainerConfig> load_trainer_config(const YAML::Node& root) {
    const YAML::Node n = root["training"];
    TrainerConfig c;
    c.enabled = config::opt_bool(n, "enabled", c.enabled);
    c.bootstrap_if_no_active = config::opt_bool(n, "bootstrap_if_no_active",
                                                c.bootstrap_if_no_active);
    c.auto_promote = config::opt_bool(n, "auto_promote", c.auto_promote);
    c.interval_hours = config::opt_int(n, "interval_hours", c.interval_hours);
    c.start_hour_utc = config::opt_int(n, "start_hour_utc", c.start_hour_utc);
    c.retry_minutes = config::opt_int(n, "retry_minutes", c.retry_minutes);
    c.lookback_days = config::opt_int(n, "lookback_days", c.lookback_days);
    c.max_rows = config::opt_int(n, "max_rows", c.max_rows);
    c.min_rows = config::opt_int(n, "min_rows", c.min_rows);
    c.folds = config::opt_int(n, "folds", c.folds);
    c.embargo_minutes = config::opt_int(n, "embargo_minutes", c.embargo_minutes);
    c.validation_fraction = config::opt_double(n, "validation_fraction", c.validation_fraction);
    c.learning_rate = config::opt_double(n, "learning_rate", c.learning_rate);
    c.num_leaves = config::opt_int(n, "num_leaves", c.num_leaves);
    c.min_data_in_leaf = config::opt_int(n, "min_data_in_leaf", c.min_data_in_leaf);
    c.max_depth = config::opt_int(n, "max_depth", c.max_depth);
    c.feature_fraction = config::opt_double(n, "feature_fraction", c.feature_fraction);
    c.bagging_fraction = config::opt_double(n, "bagging_fraction", c.bagging_fraction);
    c.bagging_freq = config::opt_int(n, "bagging_freq", c.bagging_freq);
    c.lambda_l1 = config::opt_double(n, "lambda_l1", c.lambda_l1);
    c.lambda_l2 = config::opt_double(n, "lambda_l2", c.lambda_l2);
    c.max_iterations = config::opt_int(n, "max_iterations", c.max_iterations);
    c.early_stopping_rounds = config::opt_int(n, "early_stopping_rounds", c.early_stopping_rounds);
    c.min_iterations = config::opt_int(n, "min_iterations", c.min_iterations);
    c.threads = config::opt_int(n, "threads", c.threads);
    c.time_budget_sec = config::opt_int(n, "time_budget_sec", c.time_budget_sec);
    c.max_matrix_mib = config::opt_int(n, "max_matrix_mib", c.max_matrix_mib);
    c.min_acceptance_rate = config::opt_double(n, "min_acceptance_rate", c.min_acceptance_rate);
    c.max_acceptance_rate = config::opt_double(n, "max_acceptance_rate", c.max_acceptance_rate);
    c.min_accepted = config::opt_int(n, "min_accepted", c.min_accepted);
    c.min_calibration_rows = config::opt_int(n, "min_calibration_rows", c.min_calibration_rows);
    c.min_applicable_rows = config::opt_int(n, "min_applicable_rows", c.min_applicable_rows);
    c.min_mean_fold_auc = config::opt_double(n, "min_mean_fold_auc", c.min_mean_fold_auc);
    c.min_positive_folds = config::opt_int(n, "min_positive_folds", c.min_positive_folds);
    c.min_return_per_candidate_uplift =
        config::opt_double(n, "min_return_per_candidate_uplift",
                           c.min_return_per_candidate_uplift);
    c.min_win_rate_uplift = config::opt_double(n, "min_win_rate_uplift", c.min_win_rate_uplift);
    c.max_ece = config::opt_double(n, "max_ece", c.max_ece);
    c.min_stability_share = config::opt_double(n, "min_stability_share", c.min_stability_share);
    c.stability_min_bucket_samples =
        config::opt_int(n, "stability_min_bucket_samples", c.stability_min_bucket_samples);
    c.max_load_average = config::opt_double(n, "max_load_average", c.max_load_average);
    c.max_cpu_pressure_pct =
        config::opt_double(n, "max_cpu_pressure_pct", c.max_cpu_pressure_pct);
    c.min_free_mem_mib = config::opt_int(n, "min_free_mem_mib", c.min_free_mem_mib);
    c.min_uplift_lower_bound =
        config::opt_double(n, "min_uplift_lower_bound", c.min_uplift_lower_bound);
    c.bootstrap_confidence = config::opt_double(n, "bootstrap_confidence",
                                                c.bootstrap_confidence);
    c.bootstrap_replicates = config::opt_int(n, "bootstrap_replicates",
                                             c.bootstrap_replicates);
    c.min_positive_rate = config::opt_double(n, "min_positive_rate", c.min_positive_rate);
    c.max_positive_rate = config::opt_double(n, "max_positive_rate", c.max_positive_rate);
    c.min_informative_feature_share =
        config::opt_double(n, "min_informative_feature_share",
                           c.min_informative_feature_share);
    c.evaluate_on_recommended_only =
        config::opt_bool(root["policy"], "require_upstream_recommendation",
                         c.evaluate_on_recommended_only);

    if (c.lookback_days <= 0 || c.max_rows <= 0 || c.min_rows <= 0 || c.folds < 1 ||
        c.folds > 10 || c.embargo_minutes < 0 || c.validation_fraction <= 0.0 ||
        c.validation_fraction >= 0.9 || c.learning_rate <= 0.0 || c.num_leaves < 2 ||
        c.min_data_in_leaf < 1 || c.max_depth < 1 || c.feature_fraction <= 0.0 ||
        c.feature_fraction > 1.0 || c.bagging_fraction <= 0.0 || c.bagging_fraction > 1.0 ||
        c.lambda_l1 < 0.0 || c.lambda_l2 < 0.0 || c.max_iterations < 1 || c.threads < 1 ||
        c.threads > 16 || c.time_budget_sec < 30 || c.max_matrix_mib < 16 ||
        c.max_cpu_pressure_pct <= 0.0 || c.max_cpu_pressure_pct > 100.0 ||
        c.min_acceptance_rate < 0.0 || c.min_acceptance_rate >= 1.0 ||
        c.max_acceptance_rate <= c.min_acceptance_rate || c.max_acceptance_rate > 1.0 ||
        c.min_accepted < 1 || c.min_positive_folds < 0 || c.min_positive_folds > c.folds ||
        c.max_ece < 0.0 || c.min_stability_share < 0.0 || c.min_stability_share > 1.0 ||
        c.bootstrap_confidence <= 0.5 || c.bootstrap_confidence >= 1.0 ||
        c.bootstrap_replicates < 100 || c.min_positive_rate <= 0.0 ||
        c.max_positive_rate >= 1.0 || c.min_positive_rate >= c.max_positive_rate ||
        c.min_informative_feature_share < 0.0 || c.min_informative_feature_share > 1.0) {
        return fail(ErrorCode::config_error, "invalid training configuration bounds");
    }
    return c;
}

// -------------------------------------------------------------- resource gate

double read_load_average() {
    std::ifstream in("/proc/loadavg");
    double one_min = 0;
    if (in >> one_min) return one_min;
    return 0.0;
}

int64_t read_available_mem_mib() {
    std::ifstream in("/proc/meminfo");
    std::string key, unit;
    int64_t value_kib = 0;
    while (in >> key >> value_kib >> unit) {
        if (key == "MemAvailable:") return value_kib / 1024;
    }
    return std::numeric_limits<int64_t>::max();  // unknown -> never block
}

// PSI CPU "full" 60-second average, as a percentage. Absent on kernels built
// without CONFIG_PSI, which is why the load average survives as a fallback.
std::optional<double> read_cpu_pressure_full_avg60() {
    std::ifstream in("/proc/pressure/cpu");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("full ", 0) != 0) continue;
        const auto at = line.find("avg60=");
        if (at == std::string::npos) return std::nullopt;
        try {
            return std::stod(line.substr(at + 6));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> resource_pressure(const TrainerConfig& cfg) {
    if (const auto psi = read_cpu_pressure_full_avg60()) {
        if (*psi > cfg.max_cpu_pressure_pct) {
            return "cpu_pressure_full_avg60 " + std::to_string(*psi) + "% > " +
                   std::to_string(cfg.max_cpu_pressure_pct) + "%";
        }
    } else {
        const double load = read_load_average();
        if (load > cfg.max_load_average) {
            return "load_average " + std::to_string(load) + " > " +
                   std::to_string(cfg.max_load_average) + " (PSI unavailable)";
        }
    }
    const int64_t avail = read_available_mem_mib();
    if (avail < cfg.min_free_mem_mib) {
        return "free_mem_mib " + std::to_string(avail) + " < " +
               std::to_string(cfg.min_free_mem_mib);
    }
    return std::nullopt;
}

// --------------------------------------------------------------- LightGBM RAII

struct LgbmDataset {
    DatasetHandle handle = nullptr;
    ~LgbmDataset() {
        if (handle != nullptr) LGBM_DatasetFree(handle);
    }
};

struct LgbmBooster {
    BoosterHandle handle = nullptr;
    int best_iteration = 0;
    double best_valid_metric = std::nan("");
    ~LgbmBooster() {
        if (handle != nullptr) LGBM_BoosterFree(handle);
    }
};

std::string lgbm_params(const TrainerConfig& cfg) {
    std::ostringstream out;
    out << "objective=binary metric=auc learning_rate=" << cfg.learning_rate
        << " num_leaves=" << cfg.num_leaves << " min_data_in_leaf=" << cfg.min_data_in_leaf
        << " max_depth=" << cfg.max_depth << " feature_fraction=" << cfg.feature_fraction
        << " bagging_fraction=" << cfg.bagging_fraction << " bagging_freq=" << cfg.bagging_freq
        << " lambda_l1=" << cfg.lambda_l1 << " lambda_l2=" << cfg.lambda_l2
        << " num_threads=" << cfg.threads
        // A fixed seed and a fixed thread count are what make a cycle
        // reproducible: the same dataset must yield the same model file.
        << " deterministic=true force_row_wise=true verbosity=-1 seed=42 max_bin=127";
    return out.str();
}

// The dataset in the shape LightGBM and the metrics both want.
struct TrainingData {
    std::vector<float> X;          // row-major, rows * kFeatureCount
    std::vector<int> y;
    std::vector<double> net_return;
    std::vector<domain::RowTimes> times;
    std::vector<std::string> signal_ids;
    std::vector<std::string> strategy;
    std::vector<std::string> symbol;
    std::vector<std::string> regime;
    std::vector<std::string> month;
    std::vector<std::string> side;
    // True when the live policy would let this signal reach the model at all.
    std::vector<char> applicable;
    [[nodiscard]] size_t rows() const { return y.size(); }
};

std::vector<float> gather_rows(const TrainingData& data, const std::vector<int64_t>& idx) {
    const size_t n_feat = domain::kFeatureCount;
    std::vector<float> out(idx.size() * n_feat);
    for (size_t r = 0; r < idx.size(); ++r) {
        const size_t src = static_cast<size_t>(idx[r]) * n_feat;
        std::copy_n(data.X.begin() + static_cast<std::ptrdiff_t>(src), n_feat,
                    out.begin() + static_cast<std::ptrdiff_t>(r * n_feat));
    }
    return out;
}

Result<void> make_dataset(const TrainingData& data, const std::vector<int64_t>& idx,
                          const std::string& params, DatasetHandle reference, LgbmDataset& out) {
    const auto rows = gather_rows(data, idx);
    if (LGBM_DatasetCreateFromMat(rows.data(), C_API_DTYPE_FLOAT32,
                                  static_cast<int32_t>(idx.size()),
                                  static_cast<int32_t>(domain::kFeatureCount), 1, params.c_str(),
                                  reference, &out.handle) != 0) {
        return fail(ErrorCode::model_error,
                    std::string("LightGBM dataset creation failed: ") + LGBM_GetLastError());
    }
    std::vector<float> labels(idx.size());
    for (size_t i = 0; i < idx.size(); ++i) {
        labels[i] = static_cast<float>(data.y[static_cast<size_t>(idx[i])]);
    }
    if (LGBM_DatasetSetField(out.handle, "label", labels.data(),
                             static_cast<int>(labels.size()), C_API_DTYPE_FLOAT32) != 0) {
        return fail(ErrorCode::model_error,
                    std::string("LightGBM label assignment failed: ") + LGBM_GetLastError());
    }
    return {};
}

Result<void> require_both_classes(const TrainingData& data, const std::vector<int64_t>& idx,
                                  const std::string& context) {
    bool pos = false, neg = false;
    for (const int64_t i : idx) {
        (data.y[static_cast<size_t>(i)] == 1 ? pos : neg) = true;
        if (pos && neg) return {};
    }
    return fail(ErrorCode::validation_error,
                context + " contains only one outcome class: there is nothing to learn from it");
}

Result<void> train_booster(const TrainingData& data, const std::vector<int64_t>& train_idx,
                           const std::vector<int64_t>& valid_idx, const TrainerConfig& cfg,
                           std::chrono::steady_clock::time_point deadline,
                           int64_t fixed_iterations, LgbmBooster& out) {
    const std::string params = lgbm_params(cfg);
    LgbmDataset train_set;
    if (auto r = make_dataset(data, train_idx, params, nullptr, train_set); !r) return r;
    LgbmDataset valid_set;
    const bool with_valid = !valid_idx.empty() && fixed_iterations <= 0;
    if (with_valid) {
        if (auto r = make_dataset(data, valid_idx, params, train_set.handle, valid_set); !r) {
            return r;
        }
    }
    if (LGBM_BoosterCreate(train_set.handle, params.c_str(), &out.handle) != 0) {
        return fail(ErrorCode::model_error,
                    std::string("LightGBM booster creation failed: ") + LGBM_GetLastError());
    }
    if (with_valid && LGBM_BoosterAddValidData(out.handle, valid_set.handle) != 0) {
        return fail(ErrorCode::model_error,
                    std::string("LightGBM validation set failed: ") + LGBM_GetLastError());
    }

    const int64_t max_iter = fixed_iterations > 0 ? fixed_iterations : cfg.max_iterations;
    double best = -1.0;
    int best_iter = 0, since_best = 0;
    for (int64_t iter = 1; iter <= max_iter; ++iter) {
        if (ShutdownToken::instance().stop_requested()) {
            return fail(ErrorCode::shutdown_requested, "shutdown during boosting", true);
        }
        int finished = 0;
        if (LGBM_BoosterUpdateOneIter(out.handle, &finished) != 0) {
            return fail(ErrorCode::model_error,
                        std::string("LightGBM iteration failed: ") + LGBM_GetLastError());
        }
        if (with_valid) {
            int out_len = 0;
            double metric = 0;
            if (LGBM_BoosterGetEval(out.handle, 1, &out_len, &metric) != 0 || out_len < 1) {
                return fail(ErrorCode::model_error,
                            std::string("LightGBM evaluation failed: ") + LGBM_GetLastError());
            }
            if (metric > best) {
                best = metric;
                best_iter = static_cast<int>(iter);
                since_best = 0;
            } else if (++since_best >= cfg.early_stopping_rounds) {
                break;
            }
        } else {
            best_iter = static_cast<int>(iter);
        }
        if (finished != 0) break;
        if (std::chrono::steady_clock::now() > deadline) {
            if (best_iter >= cfg.min_iterations) {
                log::warn("training_time_budget_reached", {{"kept_iterations", best_iter}});
                break;
            }
            return fail(ErrorCode::timeout, "time budget exhausted before minimum iterations");
        }
    }
    if (best_iter < 1) return fail(ErrorCode::model_error, "no boosting iterations completed");
    out.best_iteration = best_iter;
    out.best_valid_metric = with_valid ? best : std::nan("");
    return {};
}

Result<std::vector<double>> predict_rows(BoosterHandle booster, const TrainingData& data,
                                         const std::vector<int64_t>& idx, int num_iteration,
                                         const TrainerConfig& cfg) {
    const auto rows = gather_rows(data, idx);
    std::vector<double> out(idx.size());
    int64_t out_len = 0;
    const std::string params = "num_threads=" + std::to_string(cfg.threads);
    if (LGBM_BoosterPredictForMat(booster, rows.data(), C_API_DTYPE_FLOAT32,
                                  static_cast<int32_t>(idx.size()),
                                  static_cast<int32_t>(domain::kFeatureCount), 1,
                                  C_API_PREDICT_NORMAL, 0, num_iteration, params.c_str(),
                                  &out_len, out.data()) != 0 ||
        out_len != static_cast<int64_t>(idx.size())) {
        return fail(ErrorCode::model_error,
                    std::string("LightGBM prediction failed: ") + LGBM_GetLastError());
    }
    return out;
}

// Project a slice of the dataset onto the arrays the filter metrics consume.
struct Slice {
    std::vector<int> y;
    std::vector<double> net_return;
    std::vector<std::string> strategy, symbol, regime, month, side;
    std::vector<char> applicable;
};

Slice project(const TrainingData& data, const std::vector<int64_t>& idx) {
    Slice s;
    s.y.reserve(idx.size());
    s.net_return.reserve(idx.size());
    for (const int64_t i : idx) {
        const auto k = static_cast<size_t>(i);
        s.y.push_back(data.y[k]);
        s.net_return.push_back(data.net_return[k]);
        s.strategy.push_back(data.strategy[k]);
        s.symbol.push_back(data.symbol[k]);
        s.regime.push_back(data.regime[k]);
        s.month.push_back(data.month[k]);
        s.side.push_back(data.side[k]);
        s.applicable.push_back(data.applicable[k]);
    }
    return s;
}

// The rows of a slice the live policy would actually put in front of the model,
// paired with their scores. Every threshold search, every gate and every
// stability view runs on this projection, never on the whole slice.
struct ApplicableView {
    std::vector<int> y;
    std::vector<double> score;
    std::vector<double> net_return;
    std::vector<std::string> strategy, symbol, regime, month, side;
    [[nodiscard]] size_t size() const { return y.size(); }
};

ApplicableView applicable_view(const Slice& slice, const std::vector<double>& score) {
    ApplicableView v;
    for (size_t i = 0; i < slice.y.size(); ++i) {
        if (slice.applicable[i] == 0) continue;
        v.y.push_back(slice.y[i]);
        v.score.push_back(score[i]);
        v.net_return.push_back(slice.net_return[i]);
        v.strategy.push_back(slice.strategy[i]);
        v.symbol.push_back(slice.symbol[i]);
        v.regime.push_back(slice.regime[i]);
        v.month.push_back(slice.month[i]);
        v.side.push_back(slice.side[i]);
    }
    return v;
}

json interval_json(const domain::Interval& i) {
    const auto num = [](double v) { return std::isfinite(v) ? json(v) : json(nullptr); };
    return json{{"point", num(i.point)},
                {"lower", num(i.lower)},
                {"upper", num(i.upper)},
                {"samples", i.samples},
                {"block_length", i.block_length}};
}

// The contract's declared inputs versus what the dataset actually contains.
json diagnostics_json(const domain::FeatureDiagnostics& d) {
    json dead = json::array();
    for (const auto& st : d.stats) {
        if (!st.all_missing && !st.constant && st.duplicate_of.empty()) continue;
        dead.push_back({{"position", st.position},
                        {"name", st.name},
                        {"reason", st.all_missing      ? "all_missing"
                                   : st.constant       ? "constant"
                                                       : "duplicate"},
                        {"constant_value",
                         st.constant && std::isfinite(st.min_value) ? json(st.min_value)
                                                                    : json(nullptr)},
                        {"duplicate_of", st.duplicate_of}});
    }
    return json{{"declared", static_cast<int64_t>(domain::kFeatureCount)},
                {"informative", d.informative},
                {"constant", d.constant},
                {"all_missing", d.all_missing},
                {"duplicates", d.duplicates},
                {"informative_share",
                 std::isfinite(d.informative_share()) ? json(d.informative_share())
                                                      : json(nullptr)},
                {"uninformative", dead}};
}

json filter_metrics_json(const domain::FilterEvaluation& e) {
    const auto num = [](double v) { return std::isfinite(v) ? json(v) : json(nullptr); };
    return json{{"threshold", e.threshold},
                {"acceptance_rate", e.acceptance_rate},
                {"accepted_count", e.accepted.count},
                {"accepted_win_rate", num(e.accepted.win_rate)},
                {"accepted_expectancy", num(e.accepted.expectancy)},
                {"accepted_profit_factor", num(e.accepted.profit_factor)},
                {"accepted_return_per_candidate", num(e.accepted.return_per_candidate)},
                {"baseline_count", e.baseline.count},
                {"baseline_win_rate", num(e.baseline.win_rate)},
                {"baseline_expectancy", num(e.baseline.expectancy)},
                {"baseline_return_per_candidate", num(e.baseline.return_per_candidate)},
                {"rejected_count", e.rejected.count},
                {"rejected_expectancy", num(e.rejected.expectancy)},
                {"expectancy_uplift", num(e.expectancy_uplift)},
                {"win_rate_uplift", num(e.win_rate_uplift)},
                {"return_per_candidate_uplift", num(e.return_per_candidate_uplift)},
                {"roc_auc", num(e.roc_auc)},
                {"pr_auc", num(e.pr_auc)},
                {"log_loss", num(e.log_loss)},
                {"brier", num(e.brier)},
                {"ece", num(e.ece)}};
}

std::string random_hex8() {
    std::random_device rd;
    std::uniform_int_distribution<uint32_t> dist;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", dist(rd));
    return buf;
}

std::string utc_stamp() {
    std::string iso = timeutil::to_iso8601(timeutil::now_ms());  // 2026-08-23T03:00:00.000Z
    std::string out;
    for (const char ch : iso) {
        if (ch == '-' || ch == ':') continue;
        if (ch == '.') break;
        out += ch;
    }
    return out + "Z";
}

// ------------------------------------------------------------------ the cycle

struct CycleOutcome {
    std::string model_id;
    std::string status;   // ACTIVE | APPROVED | REJECTED
    std::string detail;
    int64_t rows = 0;
    // Carried out so §57 ablation can report a run without re-reading the
    // registry, and so the numbers it prints are the ones the gates saw.
    domain::FilterEvaluation evaluation;
};

Result<TrainingData> load_training_data(data::Store& store, const TrainerConfig& cfg) {
    const int64_t from_ms = timeutil::now_ms() - cfg.lookback_days * timeutil::kMsPerDay;
    auto rows = store.load_dataset(std::string(domain::kFeatureSchemaVersion),
                                   std::string(domain::kLabelSpecVersion), from_ms, cfg.max_rows);
    if (!rows) return std::unexpected(rows.error());

    const int64_t matrix_bytes =
        static_cast<int64_t>(rows->size()) * static_cast<int64_t>(domain::kFeatureCount) * 4;
    if (matrix_bytes * 4 > cfg.max_matrix_mib * 1024 * 1024) {
        return fail(ErrorCode::validation_error,
                    "estimated training memory exceeds training.max_matrix_mib");
    }

    TrainingData data;
    data.X.reserve(rows->size() * domain::kFeatureCount);
    for (const auto& r : *rows) {
        data.X.insert(data.X.end(), r.features.begin(), r.features.end());
        data.y.push_back(r.label);
        data.net_return.push_back(r.net_return);
        data.times.push_back({r.observation_time_ms, r.label_matured_at_ms});
        data.signal_ids.push_back(r.signal_proposal_id);
        data.strategy.push_back(r.strategy_id);
        data.symbol.push_back(r.symbol);
        data.regime.push_back(r.regime.empty() ? std::string("UNKNOWN") : r.regime);
        data.month.push_back(timeutil::to_iso8601(r.observation_time_ms).substr(0, 7));
        data.side.push_back(r.side);
        data.applicable.push_back(
            cfg.evaluate_on_recommended_only ? (r.upstream_recommended ? 1 : 0) : 1);
    }
    return data;
}

// The four stability views the gates read. A filter must not be carried by one
// strategy, one symbol, one regime or one week.
json stability_json(const ApplicableView& view, double threshold, const TrainerConfig& cfg,
                    double* worst_share_out) {
    json out = json::object();
    double worst_share = 1.0;
    const std::vector<std::pair<const char*, const std::vector<std::string>*>> views = {
        {"strategy", &view.strategy},
        {"symbol", &view.symbol},
        {"regime", &view.regime},
        {"month", &view.month},
        // Direction is its own view on purpose. This stack has already seen a
        // book where one side carried the whole result and the other lost money
        // in every regime; an aggregate that hides that is worse than no
        // aggregate, because it reads as evidence.
        {"side", &view.side}};
    for (const auto& [name, bucket] : views) {
        const auto s = domain::stability_by_bucket(view.y, view.score, view.net_return, *bucket,
                                                   threshold, cfg.stability_min_bucket_samples);
        out[name] = {{"buckets_evaluated", s.buckets_evaluated},
                     {"buckets_non_negative", s.buckets_non_negative},
                     {"non_negative_share",
                      std::isfinite(s.non_negative_share) ? json(s.non_negative_share)
                                                          : json(nullptr)},
                     {"worst_uplift",
                      std::isfinite(s.worst_uplift) ? json(s.worst_uplift) : json(nullptr)},
                     {"worst_bucket", s.worst_bucket}};
        // A view with too few populated buckets says nothing; it must not be
        // able to fail the gate, and it must not be able to pass it either.
        if (s.buckets_evaluated >= 2 && std::isfinite(s.non_negative_share)) {
            worst_share = std::min(worst_share, s.non_negative_share);
        }
    }
    if (worst_share_out != nullptr) *worst_share_out = worst_share;
    return out;
}

Result<CycleOutcome> run_cycle(data::Store& store, const config::RuntimeConfig& rt,
                               const TrainerConfig& cfg, const std::string& run_id) {
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(cfg.time_budget_sec);
    const std::string objective = rt.wiring.objective;

    // ---------------------------------------------------------------- data
    auto data_res = load_training_data(store, cfg);
    if (!data_res) return std::unexpected(data_res.error());
    TrainingData& data = *data_res;
    if (static_cast<int64_t>(data.rows()) < cfg.min_rows) {
        // Not enough matured history is the FIRST state of every deployment
        // (§72): the stack archives signals, waits for their outcomes and trains
        // when there are enough. Reporting it as a validation error made a
        // correct cold start look like a broken trainer in the run history and
        // in the journal, at error level, on every cycle.
        return fail(ErrorCode::not_found,
                    "insufficient matured signal history: rows=" + std::to_string(data.rows()) +
                        " min_rows=" + std::to_string(cfg.min_rows),
                    true);
    }
    const int64_t positives = std::count(data.y.begin(), data.y.end(), 1);
    const double positive_rate =
        static_cast<double>(positives) / static_cast<double>(data.rows());
    if (positive_rate < cfg.min_positive_rate || positive_rate > cfg.max_positive_rate) {
        return fail(ErrorCode::validation_error,
                    "dataset is effectively single-class: positive rate " +
                        std::to_string(positive_rate) + " outside [" +
                        std::to_string(cfg.min_positive_rate) + ", " +
                        std::to_string(cfg.max_positive_rate) + "]");
    }
    // §57 ablation: blank whole input groups so their contribution can be
    // measured rather than assumed.
    if (!cfg.ablate_groups.empty()) {
        const auto masked = domain::ablate(data.X, data.rows(), cfg.ablate_groups);
        log::info("ablation_applied", {{"groups", json(cfg.ablate_groups).dump()},
                                       {"features_blanked", masked}});
    }

    const int64_t applicable_rows =
        std::count(data.applicable.begin(), data.applicable.end(), 1);

    // Not enough of the population policy would forward. Reported as its own
    // condition, BEFORE any fold runs, because it is not a failure: the stack
    // has simply not produced enough recommended signals yet.
    //
    // Without this the cycle ran to fold 0, failed to choose a threshold on a
    // handful of applicable rows, and recorded a `failed` run -- 125 of them,
    // every one saying "no usable threshold ... (1 of 2312 rows)". An operator
    // reading that sees a broken trainer; the truth is an immature dataset.
    if (applicable_rows < cfg.min_applicable_rows) {
        return fail(ErrorCode::not_found,
                    "insufficient applicable population: " + std::to_string(applicable_rows) +
                        " of " + std::to_string(data.rows()) + " rows are applicable under the " +
                        std::string(cfg.evaluate_on_recommended_only ? "upstream_recommended_only"
                                                                     : "all") +
                        " policy, minimum " + std::to_string(cfg.min_applicable_rows));
    }

    // What the contract's 110 declared inputs actually contain here. Measured,
    // reported and logged every cycle — a disconnected upstream source shows up
    // as an all-missing block rather than as a quietly weaker model.
    const auto diagnostics = domain::diagnose_features(data.X, data.rows());
    log::info("training_feature_diagnostics",
              {{"informative", diagnostics.informative},
               {"constant", diagnostics.constant},
               {"all_missing", diagnostics.all_missing},
               {"duplicates", diagnostics.duplicates},
               {"declared", static_cast<int64_t>(domain::kFeatureCount)}});
    log::info("training_started", {{"run_id", run_id},
                                   {"rows", static_cast<int64_t>(data.rows())},
                                   {"positives", positives},
                                   {"positive_rate", positive_rate},
                                   {"applicable_rows", applicable_rows},
                                   {"features", static_cast<int64_t>(domain::kFeatureCount)},
                                   {"folds", cfg.folds},
                                   {"embargo_minutes", cfg.embargo_minutes}});

    // ------------------------------------------------------------- leakage
    // Fatal by design. A model trained on the future looks excellent in every
    // report and loses money in production, so a finding stops the cycle before
    // a single tree is grown.
    domain::LeakageReport leak;
    domain::merge(leak, domain::audit_feature_contract());
    domain::merge(leak, domain::audit_rows(data.times, data.signal_ids));
    if (!leak.clean()) {
        return fail(ErrorCode::validation_error,
                    "leakage audit failed: " + leak.findings.front() + " (and " +
                        std::to_string(leak.findings.size() - 1) + " more)");
    }

    const int64_t embargo_ms = cfg.embargo_minutes * timeutil::kMsPerMinute;
    std::vector<domain::Fold> folds;
    domain::FinalSplit final_split;
    // A training slice smaller than one leaf cannot train anything, so the split
    // enforces it rather than letting a two-row fold reach LightGBM.
    const auto min_train_rows = static_cast<size_t>(cfg.min_data_in_leaf);
    try {
        folds = domain::build_walk_forward_folds(data.times, static_cast<size_t>(cfg.folds),
                                                 embargo_ms, min_train_rows);
        final_split = domain::build_final_split(data.times, static_cast<size_t>(cfg.folds),
                                                embargo_ms, cfg.validation_fraction,
                                                min_train_rows);
    } catch (const domain::InsufficientHistory& e) {
        // The archive is too short for a leakage-safe split -- the same class of
        // state as `min_rows` and `min_applicable_rows` above, and reported the
        // same way. It was recorded as a FAILED cycle instead, which is how a
        // correct cold start came to look like a broken trainer six times over.
        return fail(ErrorCode::not_found, std::string("temporal split: ") + e.what(), true);
    } catch (const std::exception& e) {
        return fail(ErrorCode::validation_error, std::string("temporal split: ") + e.what());
    }
    domain::merge(leak, domain::audit_folds(data.times, folds, embargo_ms));
    domain::merge(leak, domain::audit_final_split(data.times, final_split, embargo_ms));
    if (!leak.clean()) {
        return fail(ErrorCode::validation_error,
                    "split leakage audit failed: " + leak.findings.front() + " (and " +
                        std::to_string(leak.findings.size() - 1) + " more)");
    }

    domain::ThresholdSearchConfig ts;
    ts.min_acceptance_rate = cfg.min_acceptance_rate;
    ts.min_accepted = cfg.min_accepted;

    // ------------------------------------------------------- walk-forward
    // Each fold is a complete rehearsal of production: fit, calibrate on the
    // slice the fit never saw, pick a threshold there, then measure the FILTER
    // on the future slice — against the baseline of taking every signal.
    std::vector<domain::FilterEvaluation> fold_evals;
    std::vector<data::FoldRecord> fold_records;
    for (size_t k = 0; k < folds.size(); ++k) {
        const auto& f = folds[k];
        const std::string ctx = "fold " + std::to_string(k);
        if (auto r = require_both_classes(data, f.train, ctx + " train"); !r) {
            return std::unexpected(r.error());
        }
        if (auto r = require_both_classes(data, f.valid, ctx + " validation"); !r) {
            return std::unexpected(r.error());
        }
        LgbmBooster booster;
        if (auto r = train_booster(data, f.train, f.valid, cfg, deadline, 0, booster); !r) {
            return std::unexpected(r.error());
        }
        auto p_valid = predict_rows(booster.handle, data, f.valid, booster.best_iteration, cfg);
        if (!p_valid) return std::unexpected(p_valid.error());
        auto p_test = predict_rows(booster.handle, data, f.test, booster.best_iteration, cfg);
        if (!p_test) return std::unexpected(p_test.error());

        const auto valid_slice = project(data, f.valid);
        domain::Calibration cal;
        // Calibration is fitted on the whole validation slice: it maps a raw
        // score to a probability, and that mapping is a property of the model,
        // not of which signals policy forwards. The THRESHOLD is not.
        if (static_cast<int64_t>(f.valid.size()) >= cfg.min_calibration_rows) {
            cal = domain::fit_isotonic(*p_valid, valid_slice.y, "fold" + std::to_string(k));
        }
        const auto valid_view =
            applicable_view(valid_slice, domain::apply_calibration(cal, *p_valid));
        // Too few APPLICABLE rows in this fold's validation window is not a
        // failure to find a threshold -- there is nothing to find one on.
        //
        // The whole-dataset check above is not enough: applicable rows are the
        // upstream-recommended ones, and on this stack they are concentrated in
        // an earlier period, so a dataset with 1189 of them can still present a
        // validation window holding exactly 1. That produced "no usable
        // threshold ... (1 of 3150 rows)" and a `failed` run, 125 times.
        if (static_cast<int64_t>(valid_view.y.size()) < cfg.min_applicable_rows) {
            return fail(ErrorCode::not_found,
                        ctx + ": insufficient applicable validation population (" +
                            std::to_string(valid_view.y.size()) + " of " +
                            std::to_string(valid_slice.y.size()) + " rows, minimum " +
                            std::to_string(cfg.min_applicable_rows) + ")");
        }
        const auto choice =
            domain::choose_threshold(valid_view.y, valid_view.score, valid_view.net_return, ts);
        if (!choice.found) {
            return fail(ErrorCode::validation_error,
                        ctx + ": no usable threshold on the applicable validation population (" +
                            std::to_string(valid_view.size()) + " of " +
                            std::to_string(valid_slice.y.size()) + " rows): " + choice.rejection);
        }
        const auto test_slice = project(data, f.test);
        const auto test_view =
            applicable_view(test_slice, domain::apply_calibration(cal, *p_test));
        const auto eval = domain::evaluate_filter(test_view.y, test_view.score,
                                                  test_view.net_return, choice.threshold);
        log::info("training_fold_done",
                  {{"fold", static_cast<int64_t>(k)},
                   {"threshold", choice.threshold},
                   {"roc_auc", eval.roc_auc},
                   {"accepted", eval.accepted.count},
                   {"acceptance_rate", eval.acceptance_rate},
                   {"return_per_candidate_uplift", eval.return_per_candidate_uplift}});
        data::FoldRecord rec;
        rec.fold = static_cast<int32_t>(k);
        rec.train_from_ms = data.times[static_cast<size_t>(f.train.front())].observed_ms;
        rec.train_to_ms = data.times[static_cast<size_t>(f.train.back())].observed_ms;
        rec.valid_from_ms = f.valid_start_ms;
        rec.valid_to_ms = f.test_start_ms;
        rec.test_from_ms = f.test_start_ms;
        rec.test_to_ms = data.times[static_cast<size_t>(f.test.back())].observed_ms;
        rec.metrics_json = filter_metrics_json(eval).dump();
        fold_records.push_back(std::move(rec));
        fold_evals.push_back(eval);
    }

    // ------------------------------------------------------------- final fit
    // Tune on the purged train/validation split, keep the calibration and the
    // threshold that split produced, then refit on every leakage-safe row with
    // the tuned iteration count. The held-out test slice is touched once.
    if (auto r = require_both_classes(data, final_split.train, "final train"); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = require_both_classes(data, final_split.valid, "final validation"); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = require_both_classes(data, final_split.fit, "final refit"); !r) {
        return std::unexpected(r.error());
    }
    LgbmBooster tuned;
    if (auto r = train_booster(data, final_split.train, final_split.valid, cfg, deadline, 0, tuned);
        !r) {
        return std::unexpected(r.error());
    }
    auto p_cal = predict_rows(tuned.handle, data, final_split.valid, tuned.best_iteration, cfg);
    if (!p_cal) return std::unexpected(p_cal.error());
    const auto cal_slice = project(data, final_split.valid);

    domain::Calibration calibration;
    calibration.version = std::string(domain::kFeatureSchemaVersion) + "/cal-1";
    std::string calibration_note = "identity (validation slice below min_calibration_rows)";
    if (static_cast<int64_t>(final_split.valid.size()) >= cfg.min_calibration_rows) {
        calibration = domain::fit_isotonic(*p_cal, cal_slice.y, calibration.version);
        calibration_note = "isotonic on the purged validation slice";
    }
    const auto cal_scores_raw = *p_cal;
    const auto cal_scores = domain::apply_calibration(calibration, cal_scores_raw);
    const json calibration_metrics = {
        {"method", calibration.method},
        {"note", calibration_note},
        {"rows", static_cast<int64_t>(final_split.valid.size())},
        {"brier_before", domain::brier_score(cal_slice.y, cal_scores_raw)},
        {"brier_after", domain::brier_score(cal_slice.y, cal_scores)},
        {"ece_before", domain::expected_calibration_error(cal_slice.y, cal_scores_raw)},
        {"ece_after", domain::expected_calibration_error(cal_slice.y, cal_scores)}};

    const auto cal_view = applicable_view(cal_slice, cal_scores);
    const auto choice =
        domain::choose_threshold(cal_view.y, cal_view.score, cal_view.net_return, ts);
    if (!choice.found) {
        return fail(ErrorCode::validation_error,
                    "no usable threshold on the applicable final validation population (" +
                        std::to_string(cal_view.size()) + " of " +
                        std::to_string(cal_slice.y.size()) + " rows): " + choice.rejection);
    }

    LgbmBooster final_model;
    if (auto r = train_booster(data, final_split.fit, {}, cfg, deadline, tuned.best_iteration,
                               final_model);
        !r) {
        return std::unexpected(r.error());
    }
    auto p_test_raw = predict_rows(final_model.handle, data, final_split.test,
                                   final_model.best_iteration, cfg);
    if (!p_test_raw) return std::unexpected(p_test_raw.error());
    const auto test_slice = project(data, final_split.test);
    const auto test_scores = domain::apply_calibration(calibration, *p_test_raw);
    const auto test_view = applicable_view(test_slice, test_scores);
    const auto final_eval = domain::evaluate_filter(test_view.y, test_view.score,
                                                    test_view.net_return, choice.threshold);
    // The whole held-out slice, for the report only. It answers "what would the
    // filter have done if policy forwarded everything", which is useful context
    // and is NOT what any gate reads.
    const auto full_eval =
        domain::evaluate_filter(test_slice.y, test_scores, test_slice.net_return,
                                choice.threshold);
    double worst_stability_share = 1.0;
    const json stability = stability_json(test_view, choice.threshold, cfg,
                                          &worst_stability_share);

    // --------------------------------------------------- statistical uncertainty
    // The held-out edge, with an interval that respects the dependence between
    // overlapping signals. A moving block bootstrap, not an IID one: signals on
    // one instrument minutes apart under a 48h holding limit are emphatically
    // not independent draws, and the IID interval would be far too narrow.
    std::vector<char> accepted_mask;
    std::vector<double> accepted_returns;
    accepted_mask.reserve(test_view.size());
    for (size_t i = 0; i < test_view.size(); ++i) {
        const bool accept = test_view.score[i] >= choice.threshold;
        accepted_mask.push_back(accept ? 1 : 0);
        if (accept) accepted_returns.push_back(test_view.net_return[i]);
    }
    const auto uplift_ci = domain::block_bootstrap_uplift(
        test_view.net_return, accepted_mask, cfg.bootstrap_confidence,
        static_cast<int>(cfg.bootstrap_replicates));
    const auto expectancy_ci = domain::block_bootstrap_mean(
        accepted_returns, cfg.bootstrap_confidence,
        static_cast<int>(cfg.bootstrap_replicates));
    const auto win_rate_ci = domain::wilson_interval(
        static_cast<int64_t>(std::llround(final_eval.accepted.win_rate *
                                          static_cast<double>(final_eval.accepted.count))),
        final_eval.accepted.count);

    // -------------------------------------------------- promotion gates (hard)
    std::vector<std::string> gate_failures;
    double mean_auc = 0, mean_uplift = 0;
    int64_t positive_folds = 0;
    for (const auto& e : fold_evals) {
        if (std::isfinite(e.roc_auc)) mean_auc += e.roc_auc;
        if (std::isfinite(e.return_per_candidate_uplift)) {
            mean_uplift += e.return_per_candidate_uplift;
            if (e.return_per_candidate_uplift > 0) ++positive_folds;
        }
    }
    mean_auc /= static_cast<double>(fold_evals.size());
    mean_uplift /= static_cast<double>(fold_evals.size());

    if (!(mean_auc >= cfg.min_mean_fold_auc)) {
        gate_failures.push_back("mean walk-forward ROC-AUC " + std::to_string(mean_auc) +
                                " < " + std::to_string(cfg.min_mean_fold_auc));
    }
    if (positive_folds < cfg.min_positive_folds) {
        gate_failures.push_back("only " + std::to_string(positive_folds) + "/" +
                                std::to_string(fold_evals.size()) +
                                " folds improved return per candidate");
    }
    // The gate that matters: the accepted set must be worth more per candidate
    // OFFERED than taking every AltStrategy signal. A filter that merely raises
    // the mean by trading less is not an improvement.
    if (!(final_eval.return_per_candidate_uplift >= cfg.min_return_per_candidate_uplift)) {
        gate_failures.push_back("held-out return per candidate uplift " +
                                std::to_string(final_eval.return_per_candidate_uplift) +
                                " < " + std::to_string(cfg.min_return_per_candidate_uplift));
    }
    if (!(final_eval.win_rate_uplift >= cfg.min_win_rate_uplift)) {
        gate_failures.push_back("held-out win-rate uplift " +
                                std::to_string(final_eval.win_rate_uplift) + " < " +
                                std::to_string(cfg.min_win_rate_uplift));
    }
    if (!(final_eval.accepted.expectancy > 0)) {
        gate_failures.emplace_back("the accepted set loses money on the held-out slice");
    }
    if (final_eval.accepted.count < cfg.min_accepted) {
        gate_failures.push_back("only " + std::to_string(final_eval.accepted.count) +
                                " accepted signals on the held-out slice");
    }
    if (final_eval.acceptance_rate < cfg.min_acceptance_rate ||
        final_eval.acceptance_rate > cfg.max_acceptance_rate) {
        gate_failures.push_back("acceptance rate " + std::to_string(final_eval.acceptance_rate) +
                                " outside [" + std::to_string(cfg.min_acceptance_rate) + ", " +
                                std::to_string(cfg.max_acceptance_rate) + "]");
    }
    if (std::isfinite(final_eval.ece) && final_eval.ece > cfg.max_ece) {
        gate_failures.push_back("calibration error " + std::to_string(final_eval.ece) + " > " +
                                std::to_string(cfg.max_ece));
    }
    if (worst_stability_share < cfg.min_stability_share) {
        gate_failures.push_back("stability: only " + std::to_string(worst_stability_share) +
                                " of buckets non-negative in the weakest view");
    }
    // The gate reads the LOWER bound, not the point estimate. A filter whose
    // interval straddles zero has not demonstrated an edge, however good its
    // mean looks.
    if (!std::isfinite(uplift_ci.lower) || uplift_ci.lower < cfg.min_uplift_lower_bound) {
        gate_failures.push_back(
            "uplift lower confidence bound " +
            (std::isfinite(uplift_ci.lower) ? std::to_string(uplift_ci.lower)
                                            : std::string("undetermined")) +
            " < " + std::to_string(cfg.min_uplift_lower_bound) +
            " (block bootstrap, n=" + std::to_string(uplift_ci.samples) + ")");
    }
    if (std::isfinite(diagnostics.informative_share()) &&
        diagnostics.informative_share() < cfg.min_informative_feature_share) {
        gate_failures.push_back(
            "only " + std::to_string(diagnostics.informative) + " of " +
            std::to_string(domain::kFeatureCount) +
            " declared features carry information in this dataset (" +
            std::to_string(diagnostics.constant) + " constant, " +
            std::to_string(diagnostics.all_missing) + " all-missing, " +
            std::to_string(diagnostics.duplicates) + " duplicate)");
    }

    // ------------------------------------ champion-challenger, one shared slice
    std::string champion_id;
    double champion_return_per_candidate = std::nan("");
    {
        auto champion = store.load_active_model(objective);
        if (!champion) return std::unexpected(champion.error());
        if (champion->has_value()) {
            auto loaded = LoadedModel::open(**champion, fs::path(rt.wiring.model_root));
            if (!loaded) {
                // An incumbent this process cannot load cannot be compared
                // against. Say so; do not silently treat it as beaten.
                log::warn("champion_not_comparable",
                          {{"model_id", (*champion)->model_id},
                           {"message", loaded.error().message}});
            } else {
                champion_id = (*champion)->model_id;
                std::vector<double> champ_scores;
                champ_scores.reserve(final_split.test.size());
                bool ok = true;
                for (const int64_t i : final_split.test) {
                    domain::FeatureVector fv;
                    const size_t base = static_cast<size_t>(i) * domain::kFeatureCount;
                    fv.values.assign(data.X.begin() + static_cast<std::ptrdiff_t>(base),
                                     data.X.begin() +
                                         static_cast<std::ptrdiff_t>(base +
                                                                     domain::kFeatureCount));
                    auto scored = (*loaded)->score(fv);
                    if (!scored) {
                        ok = false;
                        break;
                    }
                    champ_scores.push_back(scored->quality_score);
                }
                if (ok) {
                    // Scored on the SAME applicable population, with the
                    // champion's OWN threshold and its OWN calibration.
                    const auto champ_view = applicable_view(test_slice, champ_scores);
                    const auto champ_eval = domain::evaluate_filter(
                        champ_view.y, champ_view.score, champ_view.net_return,
                        (*champion)->decision_threshold);
                    champion_return_per_candidate = champ_eval.accepted.return_per_candidate;
                }
            }
        }
    }
    if (!champion_id.empty() && std::isfinite(champion_return_per_candidate) &&
        !(final_eval.accepted.return_per_candidate > champion_return_per_candidate)) {
        gate_failures.push_back("challenger does not beat the champion on the shared holdout (" +
                                std::to_string(final_eval.accepted.return_per_candidate) +
                                " vs " + std::to_string(champion_return_per_candidate) + ")");
    }

    // ------------------------------------------------------------- artifacts
    const std::string stamp = utc_stamp();
    const std::string model_id = stamp + "-lightgbm-" + random_hex8();
    const fs::path model_root(rt.wiring.model_root);
    const fs::path tmp_dir = model_root / (".tmp-" + model_id);
    const fs::path final_dir = model_root / model_id;
    std::error_code fs_error;
    fs::create_directories(tmp_dir, fs_error);
    if (fs_error) {
        return fail(ErrorCode::internal_error,
                    "cannot create model directory: " + fs_error.message());
    }
    const std::string model_path = (tmp_dir / "model.txt").string();
    if (LGBM_BoosterSaveModel(final_model.handle, 0, final_model.best_iteration,
                              C_API_FEATURE_IMPORTANCE_SPLIT, model_path.c_str()) != 0) {
        return fail(ErrorCode::model_error,
                    std::string("LightGBM model save failed: ") + LGBM_GetLastError());
    }

    // Write/read parity. The artifact that will serve live must reproduce the
    // in-memory predictions; anything else means the file is not the model.
    std::vector<int64_t> sample_idx(
        final_split.test.begin(),
        final_split.test.begin() +
            std::min<std::ptrdiff_t>(kSampleRows,
                                     static_cast<std::ptrdiff_t>(final_split.test.size())));
    BoosterHandle reloaded = nullptr;
    int reloaded_iters = 0;
    if (LGBM_BoosterCreateFromModelfile(model_path.c_str(), &reloaded_iters, &reloaded) != 0) {
        return fail(ErrorCode::model_error,
                    std::string("the saved model does not load back: ") + LGBM_GetLastError());
    }
    auto reloaded_raw = predict_rows(reloaded, data, sample_idx, -1, cfg);
    LGBM_BoosterFree(reloaded);
    if (!reloaded_raw) return std::unexpected(reloaded_raw.error());
    for (size_t i = 0; i < sample_idx.size(); ++i) {
        if (std::abs((*reloaded_raw)[i] - (*p_test_raw)[i]) > kSelfCheckTolerance) {
            return fail(ErrorCode::validation_error,
                        "write/read parity failed at sample row " + std::to_string(i));
        }
    }

    json feature_names = json::array();
    for (const auto& n : domain::lgbm_feature_names()) feature_names.push_back(n);
    json sample = {{"rows", json::array()}, {"expected_raw", json::array()},
                   {"expected_calibrated", json::array()}};
    for (size_t i = 0; i < sample_idx.size(); ++i) {
        json row = json::array();
        const size_t base = static_cast<size_t>(sample_idx[i]) * domain::kFeatureCount;
        for (size_t f = 0; f < domain::kFeatureCount; ++f) {
            const float v = data.X[base + f];
            if (std::isfinite(v)) row.push_back(v); else row.push_back(nullptr);
        }
        sample["rows"].push_back(std::move(row));
        sample["expected_raw"].push_back((*reloaded_raw)[i]);
        sample["expected_calibrated"].push_back(calibration.apply((*reloaded_raw)[i]));
    }

    json folds_json = json::array();
    for (size_t k = 0; k < fold_evals.size(); ++k) {
        folds_json.push_back({{"fold", k}, {"metrics", filter_metrics_json(fold_evals[k])}});
    }
    const bool gates_passed = gate_failures.empty();
    const std::string dataset_version =
        "ds_" + sha256_hex([&] {
            std::string joined;
            for (const auto& id : data.signal_ids) joined += id + "\n";
            return joined;
        }()).substr(0, 24);
    const json report = {
        {"trainer_version", kTrainerVersion},
        {"objective", objective},
        {"feature_schema_version", std::string(domain::kFeatureSchemaVersion)},
        {"feature_schema_hash", domain::feature_schema_hash()},
        {"label_spec_version", std::string(domain::kLabelSpecVersion)},
        {"dataset_version", dataset_version},
        {"rows", static_cast<int64_t>(data.rows())},
        {"positives", positives},
        {"features", static_cast<int64_t>(domain::kFeatureCount)},
        {"walk_forward", folds_json},
        {"mean_fold_roc_auc", mean_auc},
        {"mean_fold_return_per_candidate_uplift", mean_uplift},
        {"positive_folds", positive_folds},
        {"final_test", filter_metrics_json(final_eval)},
        {"applicable_population",
         {{"policy", cfg.evaluate_on_recommended_only ? "upstream_recommended_only" : "all"},
          {"test_rows_applicable", static_cast<int64_t>(test_view.size())},
          {"test_rows_total", static_cast<int64_t>(test_slice.y.size())}}},
        {"final_test_whole_slice_for_reference", filter_metrics_json(full_eval)},
        {"stability", stability},
        {"uncertainty",
         {{"method", "moving block bootstrap, circular blocks"},
          {"confidence", cfg.bootstrap_confidence},
          {"replicates", cfg.bootstrap_replicates},
          {"return_per_candidate_uplift", interval_json(uplift_ci)},
          {"accepted_expectancy", interval_json(expectancy_ci)},
          {"accepted_win_rate_wilson", interval_json(win_rate_ci)}}},
        {"feature_diagnostics", diagnostics_json(diagnostics)},
        {"calibration", calibration_metrics},
        {"selected_iterations", final_model.best_iteration},
        {"decision_threshold", choice.threshold},
        {"champion", champion_id.empty() ? json(nullptr) : json(champion_id)},
        {"champion_return_per_candidate", std::isfinite(champion_return_per_candidate)
                                              ? json(champion_return_per_candidate)
                                              : json(nullptr)},
        {"gates_passed", gates_passed},
        {"gate_failures", gate_failures},
        {"protocol",
         {{"split", "chronological walk-forward with a label-maturation purge"},
          {"embargo_minutes", cfg.embargo_minutes},
          {"threshold_source", "purged validation slice, objective = return per candidate"},
          {"calibration_source", "purged validation slice, isotonic (PAVA)"},
          {"metrics_scope", "held-out test slice, touched once"},
          {"baseline", "every AltStrategy candidate in the same slice"},
          {"label_spec", json::parse(domain::label_spec_json())}}}};

    const json calibration_artifact = {{"version", calibration.version},
                                       {"method", calibration.method},
                                       {"x", calibration.x},
                                       {"y", calibration.y},
                                       {"metrics", calibration_metrics}};

    const auto write_json = [&](const char* file, const json& value) {
        std::ofstream out(tmp_dir / file);
        out << value.dump(1);
        return out.good();
    };
    if (!write_json("feature_names.json", feature_names) ||
        !write_json("calibration.json", calibration_artifact) ||
        !write_json("sample_predictions.json", sample) || !write_json("report.json", report)) {
        return fail(ErrorCode::internal_error, "cannot write model artifacts");
    }
    // Atomic publish: the directory becomes visible complete, or not at all.
    fs::rename(tmp_dir, final_dir, fs_error);
    if (fs_error) {
        return fail(ErrorCode::internal_error,
                    "cannot publish the model directory: " + fs_error.message());
    }

    std::vector<data::ArtifactRecord> artifacts;
    for (const auto& [kind, file, format] :
         {std::tuple{"model", "model.txt", "lightgbm-text"},
          {"feature_names", "feature_names.json", "json"},
          {"calibration", "calibration.json", "json"},
          {"sample_predictions", "sample_predictions.json", "json"},
          {"report", "report.json", "json"}}) {
        const std::string path = (final_dir / file).string();
        artifacts.push_back({kind, path, format, file_sha256(path),
                             static_cast<int64_t>(fs::file_size(path))});
    }
    const std::string model_checksum = artifacts.front().checksum;

    // -------------------------------------------------------------- registry
    data::ModelRegistration reg;
    reg.model_id = model_id;
    reg.objective = objective;
    reg.feature_schema_version = std::string(domain::kFeatureSchemaVersion);
    reg.feature_schema_hash = domain::feature_schema_hash();
    reg.label_spec_version = std::string(domain::kLabelSpecVersion);
    reg.dataset_version = dataset_version;
    reg.decision_threshold = choice.threshold;
    reg.calibration_version = calibration.version;
    reg.feature_count = static_cast<int32_t>(domain::kFeatureCount);
    reg.hyperparameters_json = json({{"learning_rate", cfg.learning_rate},
                                     {"num_leaves", cfg.num_leaves},
                                     {"min_data_in_leaf", cfg.min_data_in_leaf},
                                     {"max_depth", cfg.max_depth},
                                     {"feature_fraction", cfg.feature_fraction},
                                     {"bagging_fraction", cfg.bagging_fraction},
                                     {"lambda_l1", cfg.lambda_l1},
                                     {"lambda_l2", cfg.lambda_l2},
                                     {"selected_iterations", final_model.best_iteration},
                                     {"embargo_minutes", cfg.embargo_minutes},
                                     {"folds", cfg.folds},
                                     {"trainer_version", kTrainerVersion}})
                                   .dump();
    reg.train_from_ms = data.times[static_cast<size_t>(final_split.fit.front())].observed_ms;
    reg.train_to_ms = final_split.valid_start_ms;
    reg.valid_from_ms = final_split.valid_start_ms;
    reg.valid_to_ms = final_split.test_start_ms;
    reg.test_from_ms = final_split.test_start_ms;
    reg.test_to_ms = data.times[static_cast<size_t>(final_split.test.back())].observed_ms;

    std::vector<std::pair<std::string, double>> metrics = {
        {"validation:mean_fold_roc_auc", mean_auc},
        {"validation:mean_fold_return_per_candidate_uplift", mean_uplift},
        {"validation:positive_folds", static_cast<double>(positive_folds)},
        {"validation:decision_threshold", choice.threshold},
        {"validation:worst_stability_share", worst_stability_share},
        {"test:roc_auc", final_eval.roc_auc},
        {"test:pr_auc", final_eval.pr_auc},
        {"test:log_loss", final_eval.log_loss},
        {"test:brier", final_eval.brier},
        {"test:ece", final_eval.ece},
        {"test:acceptance_rate", final_eval.acceptance_rate},
        {"test:accepted_count", static_cast<double>(final_eval.accepted.count)},
        {"test:accepted_win_rate", final_eval.accepted.win_rate},
        {"test:accepted_expectancy", final_eval.accepted.expectancy},
        {"test:accepted_profit_factor", final_eval.accepted.profit_factor},
        {"test:accepted_return_per_candidate", final_eval.accepted.return_per_candidate},
        {"test:expectancy_uplift", final_eval.expectancy_uplift},
        {"test:win_rate_uplift", final_eval.win_rate_uplift},
        {"test:return_per_candidate_uplift", final_eval.return_per_candidate_uplift},
        {"test_baseline:count", static_cast<double>(final_eval.baseline.count)},
        {"test_baseline:win_rate", final_eval.baseline.win_rate},
        {"test_baseline:expectancy", final_eval.baseline.expectancy},
        {"test_baseline:return_per_candidate", final_eval.baseline.return_per_candidate},
        {"test_rejected:count", static_cast<double>(final_eval.rejected.count)},
        {"test_rejected:expectancy", final_eval.rejected.expectancy},
        {"test:uplift_ci_lower", uplift_ci.lower},
        {"test:uplift_ci_upper", uplift_ci.upper},
        {"test:accepted_expectancy_ci_lower", expectancy_ci.lower},
        {"test:accepted_expectancy_ci_upper", expectancy_ci.upper},
        {"test:accepted_win_rate_ci_lower", win_rate_ci.lower},
        {"test:accepted_win_rate_ci_upper", win_rate_ci.upper},
        {"dataset:positive_rate", positive_rate},
        {"dataset:applicable_rows", static_cast<double>(applicable_rows)},
        {"dataset:informative_features", static_cast<double>(diagnostics.informative)},
        {"dataset:constant_features", static_cast<double>(diagnostics.constant)},
        {"dataset:all_missing_features", static_cast<double>(diagnostics.all_missing)},
        {"dataset:duplicate_features", static_cast<double>(diagnostics.duplicates)}};

    if (auto r = store.record_dataset_version(
            dataset_version, reg.feature_schema_version, reg.label_spec_version,
            static_cast<int64_t>(data.rows()), positives, data.times.front().observed_ms,
            data.times.back().observed_ms,
            sha256_hex([&] {
                std::string joined;
                for (const auto& id : data.signal_ids) joined += id + "\n";
                return joined;
            }()));
        !r) {
        return std::unexpected(r.error());
    }
    if (auto r = store.register_model(reg, artifacts, fold_records, calibration,
                                      calibration_metrics.dump(), metrics,
                                      json({{"gates_passed", gates_passed},
                                            {"gate_failures", gate_failures},
                                            {"run_id", run_id}})
                                          .dump());
        !r) {
        return std::unexpected(r.error());
    }

    CycleOutcome outcome;

    outcome.evaluation = final_eval;
    outcome.model_id = model_id;
    outcome.rows = static_cast<int64_t>(data.rows());
    if (!gates_passed) {
        if (auto r = store.reject_model(model_id, json({{"gate_failures", gate_failures}}).dump());
            !r) {
            return std::unexpected(r.error());
        }
        outcome.status = "REJECTED";
        outcome.detail = gate_failures.front();
        return outcome;
    }
    if (!cfg.auto_promote) {
        if (auto r = store.approve_model(model_id); !r) return std::unexpected(r.error());
        outcome.status = "APPROVED";
        outcome.detail = "gates passed; waiting for operator activation";
        return outcome;
    }
    auto previous = store.promote_model(model_id, objective, model_checksum);
    if (!previous) return std::unexpected(previous.error());
    outcome.status = "ACTIVE";
    outcome.detail = previous->empty() ? "promoted; first model for this objective"
                                       : "promoted; retired " + *previous;
    return outcome;
}

// ---------------------------------------------------------------- scheduling

int64_t utc_hour(int64_t ms) { return (ms / timeutil::kMsPerHour) % 24; }

} // namespace

// ------------------------------------------------------------- the component

struct TrainerComponent::Impl {
    TrainerConfig cfg;
    std::optional<persistence::PgPool> pool;
    std::optional<data::Store> store;
    int64_t last_attempt_ms = 0;
};

TrainerComponent::TrainerComponent(const config::RuntimeConfig& rt)
    : rt_(rt), impl_(std::make_unique<Impl>()) {}

TrainerComponent::~TrainerComponent() = default;

Result<void> TrainerComponent::init() {
    auto cfg = load_trainer_config(rt_.root);
    if (!cfg) return std::unexpected(cfg.error());
    impl_->cfg = *cfg;
    impl_->pool.emplace(rt_.common.postgres_dsn, 1);
    if (auto probe = impl_->pool->with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); });
        !probe) {
        return fail(ErrorCode::database_error, "postgres unavailable: " + probe.error().message);
    }
    impl_->store.emplace(*impl_->pool, rt_.wiring.altstrategy_schema,
                         rt_.wiring.max_stop_distance_bps);

    // The contract must be internally consistent before anything is trained on
    // it — this is the cheapest place to catch a leaking feature declaration.
    if (const auto audit = domain::audit_feature_contract(); !audit.clean()) {
        return fail(ErrorCode::validation_error,
                    "feature contract failed its leakage audit: " + audit.findings.front());
    }
    std::error_code ec;
    fs::create_directories(rt_.wiring.model_root, ec);
    if (ec) {
        return fail(ErrorCode::config_error,
                    "model root is not usable: " + rt_.wiring.model_root + ": " + ec.message());
    }
    set_component_status(name(), impl_->cfg.enabled ? "scheduling" : "disabled");
    return {};
}

void TrainerComponent::run() {
    auto& shutdown = ShutdownToken::instance();
    auto& cfg = impl_->cfg;
    auto& store = *impl_->store;
    if (!cfg.enabled) {
        while (!shutdown.stop_requested()) shutdown.wait_for(std::chrono::seconds(60));
        return;
    }
    while (!shutdown.stop_requested()) {
        const int64_t now = timeutil::now_ms();
        bool due = false;
        std::string why;

        auto active = store.has_active_model(rt_.wiring.objective);
        auto last_train = store.last_training_ms(rt_.wiring.objective);
        if (active && last_train) {
            const bool retry_ok = now - impl_->last_attempt_ms >= cfg.retry_minutes * 60'000;
            if (!*active && cfg.bootstrap_if_no_active && retry_ok) {
                due = true;
                why = "bootstrap";
            } else if (cfg.interval_hours > 0 &&
                       now - *last_train >= cfg.interval_hours * timeutil::kMsPerHour &&
                       retry_ok &&
                       (cfg.start_hour_utc < 0 || utc_hour(now) == cfg.start_hour_utc)) {
                due = true;
                why = "scheduled";
            }
        } else {
            log::warn("trainer_schedule_check_failed",
                      {{"message", (!active ? active.error() : last_train.error()).message}});
        }

        if (due) {
            impl_->last_attempt_ms = now;
            if (const auto pressure = resource_pressure(cfg)) {
                // Live evaluation keeps priority; a deferred cycle is recorded
                // so a permanently loaded box is visible rather than mysterious.
                set_component_status(name(), "JOB_DEFERRED_RESOURCE_PRESSURE");
                log::warn("JOB_DEFERRED_RESOURCE_PRESSURE", {{"reason", *pressure}});
            } else {
                set_component_status(name(), "training (" + why + ")");
                log::info("training_cycle_started", {{"reason", why}});
                auto run_id = store.begin_training_run(why);
                const std::string id = run_id ? *run_id : std::string("run_unrecorded");
                auto outcome = run_cycle(store, rt_, cfg, id);
                if (outcome) {
                    (void)store.finish_training_run(
                        id, outcome->status == "REJECTED" ? "rejected" : "succeeded",
                        outcome->model_id, outcome->rows,
                        json({{"detail", outcome->detail}}).dump(), "");
                    set_component_status(name(), "last=" + outcome->status);
                    log::info("training_cycle_done", {{"model_id", outcome->model_id},
                                                      {"status", outcome->status},
                                                      {"rows", outcome->rows},
                                                      {"detail", outcome->detail}});
                } else if (outcome.error().code == ErrorCode::shutdown_requested) {
                    (void)store.finish_training_run(id, "failed", "", 0, "{}",
                                                    "interrupted by shutdown");
                    break;
                } else if (outcome.error().code == ErrorCode::not_found) {
                    // Not enough data yet -- a state, not a fault. `deferred`
                    // already exists in the schema for exactly this, and info
                    // level keeps an immature dataset from reading as a broken
                    // trainer in the run history.
                    (void)store.finish_training_run(id, "deferred", "", 0, "{}",
                                                    outcome.error().message);
                    set_component_status(name(), "last=DEFERRED");
                    log::info("training_cycle_deferred",
                              {{"reason", outcome.error().message}});
                } else {
                    (void)store.finish_training_run(id, "failed", "", 0, "{}",
                                                    outcome.error().message);
                    set_component_status(name(), "last=FAILED");
                    log::error("training_cycle_failed", {{"message", outcome.error().message},
                                                         {"code", outcome.error().code_name()}});
                }
            }
        } else {
            set_component_status(name(), "idle");
        }
        if (shutdown.wait_for(std::chrono::seconds(60))) break;
    }
}

namespace trainer {
Result<AblationArmResult> run_ablation_arm(const config::RuntimeConfig& rt,
                                          const std::vector<std::string>& groups,
                                          bool applicable_all) {
    auto cfg = load_trainer_config(rt.root);
    if (!cfg) return std::unexpected(cfg.error());
    cfg->ablate_groups = groups;
    if (applicable_all) cfg->evaluate_on_recommended_only = false;

    persistence::PgPool pool(rt.common.postgres_dsn, 1);
    data::Store store(pool, rt.wiring.altstrategy_schema, rt.wiring.max_stop_distance_bps);
    if (auto r = store.register_contracts(); !r) return std::unexpected(r.error());

    // Recorded like any other run, with the arm in the trigger, so an ablation
    // leaves the same audit trail a scheduled cycle does.
    auto run_id = store.begin_training_run("manual");
    const std::string id = run_id ? *run_id : std::string("run_unrecorded");
    auto outcome = run_cycle(store, rt, *cfg, id);
    if (!outcome) {
        (void)store.finish_training_run(
            id, outcome.error().code == ErrorCode::not_found ? "deferred" : "failed", "", 0, "{}",
            outcome.error().message);
        return std::unexpected(outcome.error());
    }
    (void)store.finish_training_run(id, outcome->status == "REJECTED" ? "rejected" : "succeeded",
                                    outcome->model_id, outcome->rows,
                                    json({{"detail", outcome->detail}}).dump(), "");

    AblationArmResult out;
    out.rows = outcome->rows;
    out.status = outcome->status + (applicable_all ? " (all-signals)" : "");
    out.roc_auc = outcome->evaluation.roc_auc;
    out.pr_auc = outcome->evaluation.pr_auc;
    out.brier = outcome->evaluation.brier;
    // Returns are fractions on the wire; bps is what the economics are read in.
    out.expectancy_bps = outcome->evaluation.accepted.expectancy * 10'000.0;
    out.expectancy_uplift_bps = outcome->evaluation.expectancy_uplift * 10'000.0;
    return out;
}
}  // namespace trainer

int run_training_once(const config::RuntimeConfig& rt, bool force) {
    auto cfg = load_trainer_config(rt.root);
    if (!cfg) {
        std::fprintf(stderr, "config error: %s\n", cfg.error().message.c_str());
        return 2;
    }
    if (!force) {
        if (const auto pressure = resource_pressure(*cfg)) {
            std::fprintf(stderr, "JOB_DEFERRED_RESOURCE_PRESSURE: %s\n", pressure->c_str());
            return 3;
        }
    }
    std::error_code ec;
    fs::create_directories(rt.wiring.model_root, ec);
    persistence::PgPool pool(rt.common.postgres_dsn, 1);
    data::Store store(pool, rt.wiring.altstrategy_schema, rt.wiring.max_stop_distance_bps);
    if (auto r = store.register_contracts(); !r) {
        std::fprintf(stderr, "contract registration failed: %s\n", r.error().message.c_str());
        return 1;
    }
    auto run_id = store.begin_training_run("manual");
    const std::string id = run_id ? *run_id : std::string("run_unrecorded");
    auto outcome = run_cycle(store, rt, *cfg, id);
    if (!outcome) {
        // Same classification as the scheduled cycle and the ablation arm: a
        // `not_found` outcome is "not enough data yet", a state to wait out.
        // This path recorded every one of them as a FAILED run and printed
        // "training failed", which is how a correct cold start reached the run
        // history, the operator and CI as a broken trainer.
        const bool deferred = outcome.error().code == ErrorCode::not_found;
        (void)store.finish_training_run(id, deferred ? "deferred" : "failed", "", 0, "{}",
                                        outcome.error().message);
        std::fprintf(stderr, "training %s: %s\n", deferred ? "deferred" : "failed",
                     outcome.error().message.c_str());
        return 1;
    }
    (void)store.finish_training_run(id,
                                    outcome->status == "REJECTED" ? "rejected" : "succeeded",
                                    outcome->model_id, outcome->rows,
                                    json({{"detail", outcome->detail}}).dump(), "");
    std::printf("{\"event\":\"training_done\",\"model_id\":\"%s\",\"status\":\"%s\","
                "\"rows\":%lld,\"detail\":\"%s\"}\n",
                outcome->model_id.c_str(), outcome->status.c_str(),
                static_cast<long long>(outcome->rows), outcome->detail.c_str());
    return outcome->status == "REJECTED" ? 1 : 0;
}

int run_validation(const config::RuntimeConfig& rt) {
    auto cfg = load_trainer_config(rt.root);
    if (!cfg) {
        std::fprintf(stderr, "config error: %s\n", cfg.error().message.c_str());
        return 2;
    }
    persistence::PgPool pool(rt.common.postgres_dsn, 1);
    data::Store store(pool, rt.wiring.altstrategy_schema, rt.wiring.max_stop_distance_bps);

    json report;
    std::vector<std::string> findings;
    report["feature_schema_version"] = std::string(domain::kFeatureSchemaVersion);
    report["feature_schema_hash"] = domain::feature_schema_hash();
    report["feature_count"] = static_cast<int64_t>(domain::kFeatureCount);
    report["label_spec_version"] = std::string(domain::kLabelSpecVersion);

    // 1. The contract itself.
    const auto contract_audit = domain::audit_feature_contract();
    report["contract_audit"] = contract_audit.findings;
    findings.insert(findings.end(), contract_audit.findings.begin(), contract_audit.findings.end());

    // 2. The registered contract must match the compiled one.
    if (auto r = store.register_contracts(); !r) {
        findings.push_back(r.error().message);
    }

    // 3. The upstream tables the dataset is built from.
    if (auto r = store.verify_upstream_readable(); !r) {
        findings.push_back(r.error().message);
    }

    // 4. The dataset: row invariants and a real split, exactly as a training
    //    cycle would build it. This is where a leaking join shows up.
    auto data = load_training_data(store, *cfg);
    if (!data) {
        findings.push_back("dataset load failed: " + data.error().message);
        report["dataset"] = nullptr;
    } else {
        report["dataset"] = {{"rows", static_cast<int64_t>(data->rows())},
                             {"positives", std::count(data->y.begin(), data->y.end(), 1)},
                             {"min_rows", cfg->min_rows}};
        const auto row_audit = domain::audit_rows(data->times, data->signal_ids);
        report["row_audit"] = row_audit.findings;
        findings.insert(findings.end(), row_audit.findings.begin(), row_audit.findings.end());
        if (static_cast<int64_t>(data->rows()) >= cfg->min_rows) {
            const int64_t embargo_ms = cfg->embargo_minutes * timeutil::kMsPerMinute;
            try {
                // Same viability bound as the training path, so the diagnostic
                // reports the split training would actually get, not a laxer one.
                const auto min_train = static_cast<size_t>(cfg->min_data_in_leaf);
                const auto folds = domain::build_walk_forward_folds(
                    data->times, static_cast<size_t>(cfg->folds), embargo_ms, min_train);
                const auto split = domain::build_final_split(
                    data->times, static_cast<size_t>(cfg->folds), embargo_ms,
                    cfg->validation_fraction, min_train);
                const auto fold_audit = domain::audit_folds(data->times, folds, embargo_ms);
                const auto split_audit =
                    domain::audit_final_split(data->times, split, embargo_ms);
                report["split_audit"] = json::array();
                for (const auto& f : fold_audit.findings) report["split_audit"].push_back(f);
                for (const auto& f : split_audit.findings) report["split_audit"].push_back(f);
                findings.insert(findings.end(), fold_audit.findings.begin(),
                                fold_audit.findings.end());
                findings.insert(findings.end(), split_audit.findings.begin(),
                                split_audit.findings.end());
                report["split"] = {{"folds", static_cast<int64_t>(folds.size())},
                                   {"train_rows", static_cast<int64_t>(split.fit.size())},
                                   {"valid_rows", static_cast<int64_t>(split.valid.size())},
                                   {"test_rows", static_cast<int64_t>(split.test.size())}};
            } catch (const std::exception& e) {
                findings.push_back(std::string("split construction failed: ") + e.what());
            }
        } else {
            report["split"] = nullptr;
        }
    }

    // 5. The ACTIVE model: integrity, contract match, and prediction parity
    //    against the sample it recorded at training time.
    auto active = store.load_active_model(rt.wiring.objective);
    if (!active) {
        findings.push_back("registry read failed: " + active.error().message);
    } else if (!active->has_value()) {
        report["active_model"] = nullptr;
    } else {
        auto loaded = LoadedModel::open(**active, fs::path(rt.wiring.model_root));
        if (!loaded) {
            findings.push_back("active model is unusable: " + loaded.error().message);
            report["active_model"] = {{"model_id", (*active)->model_id}, {"loadable", false}};
        } else {
            json model_report = {{"model_id", (*active)->model_id},
                                 {"loadable", true},
                                 {"decision_threshold", (*active)->decision_threshold},
                                 {"calibration", (*active)->calibration_method},
                                 {"calibration_version", (*active)->calibration_version}};
            const fs::path dir = fs::path((*active)->artifact_path).parent_path();
            std::ifstream sample_in(dir / "sample_predictions.json");
            if (!sample_in.good()) {
                findings.emplace_back("the active model has no recorded sample predictions");
            } else {
                try {
                    const json sample = json::parse(sample_in);
                    int64_t checked = 0, mismatched = 0;
                    double worst = 0;
                    for (size_t i = 0; i < sample["rows"].size(); ++i) {
                        domain::FeatureVector fv;
                        fv.values.reserve(domain::kFeatureCount);
                        for (const auto& v : sample["rows"][i]) {
                            fv.values.push_back(v.is_null()
                                                    ? std::numeric_limits<float>::quiet_NaN()
                                                    : v.get<float>());
                        }
                        if (fv.values.size() != domain::kFeatureCount) {
                            findings.emplace_back(
                                "a recorded sample row does not match the feature contract width");
                            break;
                        }
                        auto scored = (*loaded)->score(fv);
                        if (!scored) {
                            findings.push_back("sample scoring failed: " + scored.error().message);
                            break;
                        }
                        const double expected =
                            sample["expected_calibrated"][i].get<double>();
                        const double delta = std::abs(scored->quality_score - expected);
                        worst = std::max(worst, delta);
                        ++checked;
                        if (delta > 1e-9) ++mismatched;
                    }
                    model_report["sample_rows_checked"] = checked;
                    model_report["sample_rows_mismatched"] = mismatched;
                    model_report["worst_absolute_delta"] = worst;
                    if (mismatched > 0) {
                        findings.push_back(
                            "the active model no longer reproduces its recorded predictions (" +
                            std::to_string(mismatched) + "/" + std::to_string(checked) +
                            " rows differ)");
                    }
                } catch (const json::exception& e) {
                    findings.push_back(std::string("sample predictions are unreadable: ") +
                                       e.what());
                }
            }
            report["active_model"] = std::move(model_report);
        }
    }

    report["findings"] = findings;
    report["ok"] = findings.empty();
    std::printf("%s\n", report.dump(2).c_str());
    return findings.empty() ? 0 : 1;
}

} // namespace altsignalquality
