#include "altml/components/trainer.hpp"

#include <LightGBM/c_api.h>
#include <nlohmann/json.hpp>

#include "altml/components/econ_calibration.hpp"
#include "altml/components/econ_logic.hpp"
#include "altml/components/feature_recipe.hpp"
#include "altml/components/trainer_logic.hpp"
#include "altml/core/feature_contract.hpp"
#include "altml/core/hash.hpp"
#include "altml/core/logger.hpp"
#include "altml/core/pg.hpp"
#include "altml/core/readiness.hpp"
#include "altml/core/shutdown.hpp"
#include "altml/core/sysinfo.hpp"
#include "altml/core/time.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace altml {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kTrainerVersion = "native-tr-1.0.0";
// The registry's description of WHAT a model predicts. It has to be derived
// from the configured target, not fixed: this was the literal string
// "binary:y_direction_15m" while the deployed config set
// target_label: direction_30m, so every one of the 37 models in the registry
// was trained on the 30-minute label and recorded as a 15-minute one. Training
// read cfg.target_label and the registry read this -- nothing reconciled them,
// so nothing complained.
constexpr const char* kDefaultTargetLabel = "direction_15m";
constexpr const char* kTargetLabel = "direction_15m";
constexpr const char* kReturnLabel = "future_return_15m";
constexpr int64_t kHoldingMs = 15LL * 60 * 1000;
constexpr int kSampleRows = 100;
constexpr double kSelfCheckTolerance = 1e-6;

// ------------------------------------------------------------- configuration

struct TrainerConfig {
    bool enabled = true;
    // "econ" trains the ML-first multi-horizon models (econ-labels, one
    // multiclass LightGBM per horizon); "legacy" the retired direction_15m
    // binary. The decision layer serves econ models only.
    std::string mode = "econ";
    // DT-02/DT-04: the recipe a NEW model is trained on (recipe-1 or recipe-2);
    // recorded with it, and inference serves it that projection. recipe-1 is
    // the deployed contract until a recipe-2 pair passes its gates.
    std::string recipe_version = recipe::kRecipeVersion;
    std::vector<std::string> econ_horizons = {"15m", "1h", "4h"};
    // The aligned sources an econ model reads. Same spec grammar as
    // t0_sources; the column choice inside each set is the recipe's
    // (feature_recipe.hpp), not a projection list.
    //
    // The default IS the deployed contract: the Bitget 15m and 1h grids. The
    // reference venues, cross-venue, Bitget microstructure and macro blocks
    // showed no incremental OOS value on the econ target (walk-forward
    // ablation 2026-09-25) and stay candidate families -- a config that loses
    // its econ_sources key must not quietly train on them.
    std::vector<std::string> econ_sources = {"1:15m:60", "1:1h:180"};
    // Holdout day-clustered t required to promote (see econ::GateConfig).
    double econ_min_holdout_t = 2.0;
    // Cost model v2 (econ_logic.hpp), per side, bps: the impact assumption and
    // the spread used where no book reading exists at T0.
    double econ_impact_bps_per_side = 1.0;
    double econ_fallback_spread_bps = 2.0;
    // Boosting budget PER HORIZON; the data load is shared and not counted.
    int64_t econ_time_budget_sec = 1500;
    bool bootstrap_if_no_active = true;
    bool auto_promote = true;
    int64_t interval_hours = 168;   // weekly
    int64_t start_hour_utc = 3;     // -1 = any hour
    int64_t retry_minutes = 60;
    int64_t lookback_days = 93;
    int64_t sample_every_minutes = 5;
    int64_t max_rows = 1'000'000;
    int64_t min_rows = 50'000;
    int64_t folds = 4;
    int64_t embargo_minutes = 30;
    double validation_fraction = 0.15;
    // Tree shape for a noisy economic target, fixed a priori as in the
    // walk-forward research (2026-09-25) and equal to the deployed config:
    // small trees, large leaves, slow learning, patient early stopping. The
    // earlier 0.05 / 63 / 200 / 600 / 50 was sized for the retired binary
    // direction label; a config that lost these keys would have trained a
    // different, far less regularised model than the one that was evaluated.
    double learning_rate = 0.03;
    int64_t num_leaves = 15;
    int64_t min_data_in_leaf = 500;
    // Regularisation. These were hardcoded into the LightGBM parameter string
    // while their three siblings above were configurable, so the only knobs that
    // control overfitting could not be set for the data regime actually in use.
    // Defaults target the high-dimensional case this service is in: ~995
    // features against far fewer INDEPENDENT observations, because a 15m label
    // makes at most 4 non-overlapping samples per instrument-hour and crypto
    // instruments are strongly cross-correlated.
    // The prediction target. Hardcoded to direction_15m/future_return_15m, which
    // fixed BOTH the horizon and the objective. The horizon matters
    // quantitatively: trading costs are roughly fixed per round turn while
    // return dispersion grows with the square root of the horizon, so the edge a
    // model must find to clear costs SHRINKS as the horizon lengthens. Measured
    // here: sd(future_return_15m) = 29.4 bps, so ~8 bps of cost is a 0.27-sigma
    // hurdle at 15m and ~0.19-sigma at 30m. Which horizon actually pays is an
    // empirical question, and it could not even be asked while this was a
    // compile-time constant.
    std::string target_label = kDefaultTargetLabel;
    std::string return_label = "future_return_15m";

    // --- point-in-time input sources ------------------------------------
    //
    // The target's own Bitget 1m observation is always the base. These are the
    // ADDITIONAL observations aligned to the same T0: other venues, other
    // grids. Each is "exchange_id:timeframe:freshness_minutes".
    //
    // They are projected, not copied wholesale: 995 features per source across
    // 18 possible venue/grid combinations is ~19k columns against far fewer
    // independent observations, which the matrix budget rejects and statistics
    // rejects for better reasons. The projection below is the dataset's feature
    // SPACE (versioned with it); it is not statistical selection, which stays
    // fold-local.
    std::vector<std::string> t0_sources = {
        "2:1m:5", "3:1m:5", "4:1m:5",      // reference venues, same grid
        "1:5m:20", "1:15m:60", "1:1h:180", // target venue, higher grids
        // reference_lite carries what a candle cannot: spread, book depth and
        // imbalance, microprice, trade flow and CVD, funding, open interest,
        // mark and index deviation. AltTech has published it for every venue
        // and AltML has archived it all along, while the trainer read only
        // talib-core -- so the execution venue's own microstructure, the single
        // most direct evidence about the cost of acting, was never in the
        // matrix at all. Taken whole (45 columns), version pinned.
        "1:1m:5:reference_lite:4", "2:1m:5:reference_lite:4",
        "3:1m:5:reference_lite:4", "4:1m:5:reference_lite:4",
        // The consensus across all four venues, and where the execution venue
        // sits relative to it: deviation from consensus price, lead and lag
        // scores, order-flow and funding and open-interest agreement. Venue 0 --
        // it belongs to no exchange. AltTech publishes it on a subject AltML did
        // not subscribe to, so this arrives only once that subscription widens.
        "0:1m:5:cross_venue_core:2",
        // The execution venue's own book and tape, per minute: trade counts and
        // sizes, aggressive volume, delta and CVD, large-trade intensity,
        // short-term price impact, burst score, book spread, imbalance,
        // microprice deviation, per-side depth. Nothing in a candle expresses
        // any of it, and it is where the cost of acting on Bitget shows up.
        "1:1m:5:bitget_micro:1",
        // The information venues' microstructure. AltTech computes it and
        // publishes it on no NATS subject at all, so it reaches the archive only
        // through the FDW backfill -- nothing that listens to the stream could
        // ever have seen it. 11 columns each: the same book and tape questions
        // asked of Binance, Bybit and OKX.
        "2:1m:5:reference_lite_micro:4", "3:1m:5:reference_lite_micro:4",
        "4:1m:5:reference_lite_micro:4",
        // The macro / cross-asset block: AltTech's macro-core, derived from
        // AltMacro's external series -- the offshore renminbi against the PBOC
        // parity and the H.10 reference, the majors and gold, VIX and the
        // index closes, the Treasury curve. Venue 0 and instrument 0 because
        // it belongs to no exchange and no pair; the sixth field pins the join
        // to instrument 0 instead of following the target's own instrument,
        // and live inference reads the same pinned observation for every
        // target. It is part of the canonical input by decision: the model,
        // not a curated list, decides what weight each of its 40 columns gets.
        // It was walk-forward compared against the set without it before it
        // was admitted here (2026-09-12, both runs REJECTED by the standard
        // gates, the macro-augmented one lower on every metric); it is
        // included anyway because the system is to see all of its inputs and
        // let selection do the excluding.
        "0:1m:5:macro-core:1:0",
    };
    std::vector<std::string> t0_projection = {
        "RSI_14_outReal", "ATR_14_outReal", "ADX_14_outReal", "CCI_14_outReal",
        "MFI_14_outReal", "WILLR_14_outReal", "NATR_14_outReal", "OBV_outReal",
        "STDDEV_5_outReal", "LINEARREG_14_outReal", "TYPPRICE_outReal",
        "HT_DCPERIOD_outReal",
    };
    double feature_fraction = 0.3;
    double bagging_fraction = 0.7;
    int64_t bagging_freq = 1;
    double lambda_l1 = 1.0;
    double lambda_l2 = 10.0;
    int64_t max_depth = 6;
    int64_t max_iterations = 1500;
    int64_t early_stopping_rounds = 100;
    int64_t min_iterations = 20;
    int64_t threads = 2;
    int64_t time_budget_sec = 2400;
    // 900 put the row cap below min_rows once the aligned blocks were in the
    // matrix, so no cycle could run; 1200 is the deployed figure.
    int64_t max_matrix_mib = 1200;
    // Cost is ONE decision with one canonical home: altml.fee_models, the same
    // table the label builder reads for `future_return_after_fees_*`. These
    // fields are the resolved values; they are filled from that table at
    // start-up unless the config states them explicitly.
    //
    // Two independent copies of the same number is how they drift: the labels
    // would price a trade one way and the economic gate another, and nothing
    // would report the disagreement.
    int64_t fee_model_version = 1;
    double round_trip_fee_bps = 12.0;
    double round_trip_slippage_bps = 4.0;
    bool cost_from_config = false;   // true when the config pinned the numbers
    double conviction_quantile = 0.8;
    int64_t min_trades = 50;
    double min_profit_factor = 1.05;
    double max_drawdown = 0.10;
    double min_mean_fold_auc = 0.50;
    int64_t min_positive_folds = 3;
    double max_load_average = 6.0;
    int64_t min_free_mem_mib = 512;
    // Free space on the filesystem holding the database, below which a
    // training run is deferred like any other resource pressure. A run writes
    // a dataset's worth of temporary files and WAL; starting one on a nearly
    // full disk is how the 2026-09-17 outage would have been made worse. 20 GB
    // is ~10% of VPS1's 192 GB filesystem, the CRITICAL line of the host's
    // storage policy.
    int64_t min_free_disk_gb = 20;
};

Result<TrainerConfig> load_trainer_config(const YAML::Node& root) {
    const YAML::Node n = root["training"];
    TrainerConfig c;
    c.enabled = config::opt_bool(n, "enabled", c.enabled);
    c.mode = config::opt_str(n, "mode", c.mode);
    if (const auto hz = config::opt_str_list(n, "econ_horizons"); !hz.empty()) c.econ_horizons = hz;
    if (const auto src = config::opt_str_list(n, "econ_sources"); !src.empty()) c.econ_sources = src;
    if (const auto rv = config::opt_str(n, "recipe_version", c.recipe_version); !recipe::projection_for(rv).empty()) {
        c.recipe_version = rv;
    } else {
        return fail(ErrorCode::config_error, "training.recipe_version is not a recipe this build knows: " + rv);
    }
    c.econ_min_holdout_t = config::opt_double(n, "econ_min_holdout_t", c.econ_min_holdout_t);
    c.econ_time_budget_sec = config::opt_int(n, "econ_time_budget_sec", c.econ_time_budget_sec);
    c.econ_impact_bps_per_side =
        config::opt_double(n, "econ_impact_bps_per_side", c.econ_impact_bps_per_side);
    c.econ_fallback_spread_bps =
        config::opt_double(n, "econ_fallback_spread_bps", c.econ_fallback_spread_bps);
    if (!(c.econ_impact_bps_per_side >= 0.0) || !(c.econ_fallback_spread_bps > 0.0)) {
        return fail(ErrorCode::config_error, "invalid econ cost model bounds");
    }
    if (c.mode != "econ" && c.mode != "legacy") {
        return fail(ErrorCode::config_error, "training.mode must be econ or legacy");
    }
    for (const auto& h : c.econ_horizons) {
        if (!econ::horizon_minutes(h)) {
            return fail(ErrorCode::config_error, "training.econ_horizons: unknown horizon " + h);
        }
    }
    if (c.econ_time_budget_sec < 60 || !(c.econ_min_holdout_t > 0.0)) {
        return fail(ErrorCode::config_error, "invalid econ training bounds");
    }
    c.bootstrap_if_no_active = config::opt_bool(n, "bootstrap_if_no_active",
                                                c.bootstrap_if_no_active);
    c.auto_promote = config::opt_bool(n, "auto_promote", c.auto_promote);
    c.interval_hours = config::opt_int(n, "interval_hours", c.interval_hours);
    c.start_hour_utc = config::opt_int(n, "start_hour_utc", c.start_hour_utc);
    c.retry_minutes = config::opt_int(n, "retry_minutes", c.retry_minutes);
    c.lookback_days = config::opt_int(n, "lookback_days", c.lookback_days);
    c.sample_every_minutes = config::opt_int(n, "sample_every_minutes", c.sample_every_minutes);
    c.max_rows = config::opt_int(n, "max_rows", c.max_rows);
    c.min_rows = config::opt_int(n, "min_rows", c.min_rows);
    c.folds = config::opt_int(n, "folds", c.folds);
    c.embargo_minutes = config::opt_int(n, "embargo_minutes", c.embargo_minutes);
    c.validation_fraction = config::opt_double(n, "validation_fraction", c.validation_fraction);
    c.learning_rate = config::opt_double(n, "learning_rate", c.learning_rate);
    c.num_leaves = config::opt_int(n, "num_leaves", c.num_leaves);
    c.min_data_in_leaf = config::opt_int(n, "min_data_in_leaf", c.min_data_in_leaf);
    c.target_label = config::opt_str(n, "target_label", c.target_label);
    if (const auto list = config::opt_str_list(n, "t0_sources"); !list.empty()) {
        c.t0_sources = list;
    }
    if (const auto proj = config::opt_str_list(n, "t0_projection"); !proj.empty()) {
        c.t0_projection = proj;
    }
    c.return_label = config::opt_str(n, "return_label", c.return_label);
    c.feature_fraction = config::opt_double(n, "feature_fraction", c.feature_fraction);
    c.bagging_fraction = config::opt_double(n, "bagging_fraction", c.bagging_fraction);
    c.bagging_freq = config::opt_int(n, "bagging_freq", c.bagging_freq);
    c.lambda_l1 = config::opt_double(n, "lambda_l1", c.lambda_l1);
    c.lambda_l2 = config::opt_double(n, "lambda_l2", c.lambda_l2);
    c.max_depth = config::opt_int(n, "max_depth", c.max_depth);
    c.max_iterations = config::opt_int(n, "max_iterations", c.max_iterations);
    c.early_stopping_rounds = config::opt_int(n, "early_stopping_rounds",
                                              c.early_stopping_rounds);
    c.threads = config::opt_int(n, "threads", c.threads);
    c.time_budget_sec = config::opt_int(n, "time_budget_sec", c.time_budget_sec);
    c.max_matrix_mib = config::opt_int(n, "max_matrix_mib", c.max_matrix_mib);
    c.fee_model_version = config::opt_int(n, "fee_model_version", c.fee_model_version);
    c.cost_from_config = n && (n["round_trip_fee_bps"] || n["round_trip_slippage_bps"]);
    c.round_trip_fee_bps = config::opt_double(n, "round_trip_fee_bps", c.round_trip_fee_bps);
    c.round_trip_slippage_bps =
        config::opt_double(n, "round_trip_slippage_bps", c.round_trip_slippage_bps);
    c.conviction_quantile = config::opt_double(n, "conviction_quantile", c.conviction_quantile);
    c.min_trades = config::opt_int(n, "min_trades", c.min_trades);
    c.min_profit_factor = config::opt_double(n, "min_profit_factor", c.min_profit_factor);
    c.max_drawdown = config::opt_double(n, "max_drawdown", c.max_drawdown);
    c.min_mean_fold_auc = config::opt_double(n, "min_mean_fold_auc", c.min_mean_fold_auc);
    c.min_positive_folds = config::opt_int(n, "min_positive_folds", c.min_positive_folds);
    c.max_load_average = config::opt_double(n, "max_load_average", c.max_load_average);
    c.min_free_mem_mib = config::opt_int(n, "min_free_mem_mib", c.min_free_mem_mib);
    c.min_free_disk_gb = config::opt_int(n, "min_free_disk_gb", c.min_free_disk_gb);

    if (c.lookback_days <= 0 || c.sample_every_minutes <= 0 || c.max_rows <= 0 ||
        c.min_rows <= 0 || c.folds < 1 || c.folds > 12 || c.embargo_minutes < 0 ||
        c.validation_fraction <= 0.0 || c.validation_fraction >= 0.9 ||
        c.feature_fraction <= 0.0 || c.feature_fraction > 1.0 ||
        c.bagging_fraction <= 0.0 || c.bagging_fraction > 1.0 ||
        c.lambda_l1 < 0.0 || c.lambda_l2 < 0.0 || c.max_depth < 1 ||
        c.learning_rate <= 0.0 || c.num_leaves < 2 || c.max_iterations < 1 ||
        c.threads < 1 || c.threads > 16 || c.time_budget_sec < 60 ||
        c.max_matrix_mib < 64 || c.conviction_quantile < 0.0 ||
        c.conviction_quantile > 1.0 || c.min_trades < 1 ||
        c.min_positive_folds < 0 || c.min_positive_folds > c.folds) {
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
    std::string key;
    int64_t value_kib = 0;
    std::string unit;
    while (in >> key >> value_kib >> unit) {
        if (key == "MemAvailable:") return value_kib / 1024;
    }
    return std::numeric_limits<int64_t>::max(); // unknown -> do not block
}

std::optional<std::string> resource_pressure(const TrainerConfig& cfg) {
    const double load = read_load_average();
    if (load > cfg.max_load_average) {
        return "load_average " + std::to_string(load) + " > " +
               std::to_string(cfg.max_load_average);
    }
    const int64_t avail = read_available_mem_mib();
    if (avail < cfg.min_free_mem_mib) {
        return "free_mem_mib " + std::to_string(avail) + " < " +
               std::to_string(cfg.min_free_mem_mib);
    }
    const int64_t free_gb = sysinfo::free_disk_gb("/");
    if (free_gb >= 0 && free_gb < cfg.min_free_disk_gb) {
        return "free_disk_gb " + std::to_string(free_gb) + " < " +
               std::to_string(cfg.min_free_disk_gb);
    }
    return std::nullopt;
}

// --------------------------------------------------------------- data loading

struct TrainingData {
    std::vector<float> X;  // row-major, rows * n_features
    int32_t n_features = 0;
    std::vector<int> y;            // 1 up, 0 down, -1 flat sentinel
    std::vector<double> r15;       // raw 15m log return (labels, before costs)
    std::vector<int64_t> t_ms;     // bar open time
    std::vector<int32_t> sym_ids;
    std::vector<std::string> symbols;
    std::vector<std::string> feature_names;
    std::string feature_schema_hash;
    int64_t effective_sample_minutes = 0;
    int64_t candidate_rows = 0;
    // The input recipe, carried to registration so a served model is rebuilt
    // from what it was TRAINED with, not from whatever the config says later.
    int32_t base_feature_count = 0;
    std::vector<std::string> aligned_sources;
    std::vector<std::string> aligned_projection;

    [[nodiscard]] size_t rows() const { return y.size(); }
};

// Source-quality gate identical to the retired dataset builder: labels and
// features must belong to the current revision of a closed, quality-ok candle,
// be computed after the candle's last update, and the whole label horizon must
// be gap-free. Nothing here can look into the future of the anchor bar.
// Horizon completeness is NOT re-derived here. A label only carries a set
// validity bit when the builder's labels::source_range_valid() has confirmed
// that every minute in [t, t+h] is present, strictly contiguous and quality-ok,
// and the builder's revision audit recomputes any label a later candle revision
// touched. Re-deriving that per row through a correlated NOT EXISTS over the
// postgres_fdw candle table cost `max_label_horizon` remote round-trips for
// every candidate row: the preflight count hit its 120 s statement timeout on
// every cycle, so training never started and no model was ever produced. The
// validity bitmap carries the identical guarantee as a local column.
//
// Gating on the bitmap here also makes the preflight count agree with the rows
// the loader actually keeps — it applies the same two bits in C++ after the
// fetch — instead of counting rows that are then discarded.
// Split into the JOIN and the WHERE deliberately. They used to be one string,
// and that made it impossible to add a join afterwards: anything appended
// landed after WHERE, where SQL does not accept a join. The T0 sources have to
// sit between the two.
std::string quality_join_clause() {
    return " JOIN altml.fdw_candles_1m c ON c.exchange_id = l.exchange_id "
           "  AND c.instrument_id = l.instrument_id AND c.open_time = l.bar_open_time "
           "  AND c.revision = l.bar_revision ";
}

std::string quality_where_sql(size_t direction_pos, size_t return_pos) {
    return std::string(
        " WHERE c.is_closed AND c.quality_status = 'ok' "
        "AND l.computed_at >= c.updated_at "
        "AND f.quality_status = 'VALID' "
        "AND f.computed_at >= c.updated_at "
        "AND f.engine_version = '") + analytics::kFeatureEngineVersion + "' " +
        // get_bit() numbers bits from the least significant end of each byte,
        // matching the builder's `validity[i / 8] |= 1 << (i % 8)`.
        "AND get_bit(l.validity_bitmap, " + std::to_string(direction_pos) + ") = 1 " +
        "AND get_bit(l.validity_bitmap, " + std::to_string(return_pos) + ") = 1 ";
}

struct SetInfo {
    int64_t feature_set_id = 0;
    std::string feature_schema_hash;
    std::vector<std::string> feature_names;
    int64_t label_set_id = 0;
    std::vector<std::string> label_names;
    int max_label_horizon = 0;
    size_t direction_pos = 0;
    size_t return_pos = 0;
};

Result<SetInfo> resolve_sets(persistence::PgPool& pool, const TrainerConfig& cfg) {
    SetInfo info;
    std::string error;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto fs = txn.exec_params(
            "SELECT id, schema_hash, frozen, engine_version FROM altml.fdw_feature_set_versions "
            "WHERE name = $1 AND version = $2",
            std::string(analytics::kCoreFeatureSetName), analytics::kCoreFeatureSetVersion);
        if (fs.empty()) {
            error = "live feature set not found";
            return;
        }
        if (!fs[0][2].as<bool>() ||
            fs[0][3].as<std::string>() != analytics::kFeatureEngineVersion) {
            error = "feature set is not the frozen current engine";
            return;
        }
        info.feature_set_id = fs[0][0].as<int64_t>();
        info.feature_schema_hash = fs[0][1].as<std::string>();
        for (const auto& row : txn.exec_params(
                 "SELECT output_name FROM altml.fdw_feature_set_members "
                 "WHERE feature_set_version_id = $1 ORDER BY position",
                 info.feature_set_id)) {
            info.feature_names.push_back(row[0].as<std::string>());
        }
        const auto ls = txn.exec_params(
            "SELECT id, frozen FROM altml.label_set_versions WHERE name = $1 AND version = $2",
            std::string("core-labels"), 1);
        if (ls.empty() || !ls[0][1].as<bool>()) {
            error = "label set core-labels:1 missing or not frozen";
            return;
        }
        info.label_set_id = ls[0][0].as<int64_t>();
        for (const auto& row : txn.exec_params(
                 "SELECT output_name FROM altml.label_set_members "
                 "WHERE label_set_version_id = $1 ORDER BY position",
                 info.label_set_id)) {
            info.label_names.push_back(row[0].as<std::string>());
        }
        const auto horizon = txn.exec_params(
            "SELECT max(d.horizon_minutes) FROM altml.label_set_members m "
            "JOIN altml.label_definitions d ON d.id = m.definition_id "
            "WHERE m.label_set_version_id = $1",
            info.label_set_id);
        if (!horizon.empty() && !horizon[0][0].is_null()) {
            info.max_label_horizon = horizon[0][0].as<int>();
        }
    });
    if (!res) return std::unexpected(res.error());
    if (!error.empty()) return fail(ErrorCode::validation_error, error);
    if (info.feature_names.empty() || info.label_names.empty() ||
        info.max_label_horizon <= 0) {
        return fail(ErrorCode::validation_error, "empty feature/label catalog");
    }
    const auto dir = std::find(info.label_names.begin(), info.label_names.end(), cfg.target_label);
    const auto ret = std::find(info.label_names.begin(), info.label_names.end(), cfg.return_label);
    if (dir == info.label_names.end() || ret == info.label_names.end()) {
        return fail(ErrorCode::validation_error,
                    "label set lacks " + cfg.target_label + "/" + cfg.return_label);
    }
    info.direction_pos = static_cast<size_t>(dir - info.label_names.begin());
    info.return_pos = static_cast<size_t>(ret - info.label_names.begin());
    return info;
}

// Column order of any frozen feature set, by name and version. Used for sets
// other than the target's own: a source that reads reference_lite needs that
// set's layout, and the version matters because two frozen versions share a
// name and differ in columns.
struct SetLayout {
    std::vector<std::string> columns;
    std::vector<int> equivalent_versions;  // what selects rows; see AlignedSource
};

Result<SetLayout> load_set_layout(persistence::PgPool& pool,
                                  const std::string& name, int version) {
    SetLayout layout;
    std::string error;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto fs = txn.exec_params(
            "SELECT id, frozen FROM altml.fdw_feature_set_versions "
            "WHERE name = $1 AND version = $2",
            name, version);
        if (fs.empty()) {
            error = "feature set not found: " + name + ":" + std::to_string(version);
            return;
        }
        if (!fs[0][1].as<bool>()) {
            error = "feature set is not frozen: " + name + ":" + std::to_string(version);
            return;
        }
        for (const auto& row : txn.exec_params(
                 "SELECT output_name FROM altml.fdw_feature_set_members "
                 "WHERE feature_set_version_id = $1 ORDER BY position",
                 fs[0][0].as<int64_t>())) {
            layout.columns.push_back(row[0].as<std::string>());
        }
        // Every frozen version of this set whose columns are identical, in the
        // same order. Compared here rather than trusted from schema_hash, which
        // AltTech leaves empty on the wire for some sets.
        std::string joined;
        for (const auto& c : layout.columns) { joined += c; joined += '|'; }
        for (const auto& row : txn.exec_params(
                 "SELECT v.version, string_agg(m.output_name, '|' ORDER BY m.position) || '|' "
                 "FROM altml.fdw_feature_set_versions v "
                 "JOIN altml.fdw_feature_set_members m ON m.feature_set_version_id = v.id "
                 "WHERE v.name = $1 AND v.frozen GROUP BY v.version",
                 name)) {
            if (row[1].as<std::string>() == joined) {
                layout.equivalent_versions.push_back(row[0].as<int>());
            }
        }
    });
    if (!res) return std::unexpected(res.error());
    if (!error.empty()) return fail(ErrorCode::validation_error, error);
    if (layout.columns.empty()) {
        return fail(ErrorCode::validation_error, "feature set has no members: " + name);
    }
    if (layout.equivalent_versions.empty()) {
        return fail(ErrorCode::validation_error, "feature set version not resolvable: " + name);
    }
    return layout;
}

Result<TrainingData> load_training_data(persistence::PgPool& pool, const SetInfo& sets,
                                        const TrainerConfig& cfg,
                                        const std::string& exchange_code) {
    TrainingData data;
    data.n_features = static_cast<int32_t>(sets.feature_names.size());
    data.base_feature_count = data.n_features;
    data.feature_names = sets.feature_names;
    data.feature_schema_hash = sets.feature_schema_hash;

    struct Instrument {
        int64_t exchange_id;
        int64_t instrument_id;
        std::string symbol;
    };
    std::vector<Instrument> instruments;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        for (const auto& row : txn.exec_params(
                 "SELECT ei.exchange_id, ei.instrument_id, ei.exchange_symbol "
                 "FROM altml.fdw_exchange_instruments ei JOIN altml.fdw_exchanges e "
                 "ON e.id = ei.exchange_id "
                 "WHERE e.code = $1 AND ei.is_whitelisted AND ei.status = 'active' "
                 "ORDER BY ei.exchange_symbol",
                 exchange_code)) {
            instruments.push_back({row[0].as<int64_t>(), row[1].as<int64_t>(),
                                   row[2].as<std::string>()});
        }
    });
    if (!res) return std::unexpected(res.error());
    if (instruments.empty()) {
        return fail(ErrorCode::validation_error, "no whitelisted active instruments");
    }

    const int64_t now = timeutil::now_ms();
    const int64_t day_ms = 24LL * 3600 * 1000;
    const int64_t date_to = (now / day_ms) * day_ms;  // completed-day boundary (UTC)
    const int64_t date_from = date_to - cfg.lookback_days * day_ms;

    // Placeholders: $1 exchange, $2 instrument, $3 feature schema hash,
    // $4 label set, $5 date_from, $6 date_to, $7 sampling stride. The label
    // horizon is no longer a bind parameter — horizon completeness now comes
    // from the label's own validity bitmap (see quality_join_sql).
    const std::string date_cond =
        " AND f.bar_open_time >= to_timestamp($5::bigint / 1000.0) "
        "AND f.bar_open_time < to_timestamp($6::bigint / 1000.0) "
        "AND mod(floor(extract(epoch FROM f.bar_open_time) / 60)::bigint, $7) = 0 ";
    const std::string count_sql =
        "SELECT count(*) FROM altml.feature_vectors f "
        "JOIN altml.labels l ON l.exchange_id = f.exchange_id "
        " AND l.instrument_id = f.instrument_id AND l.timeframe = f.timeframe "
        " AND l.bar_open_time = f.bar_open_time AND l.bar_revision = f.bar_revision " +
        quality_join_clause() + quality_where_sql(sets.direction_pos, sets.return_pos) +
        "AND f.exchange_id = $1 AND f.instrument_id = $2 AND f.timeframe = '1m' "
        "AND f.schema_hash = $3 AND l.label_set_version_id = $4" + date_cond;

    // Preflight count with automatic sampling scale-up: autonomy means the
    // trainer coarsens sampling instead of demanding an operator retune it.
    int64_t sample_minutes = cfg.sample_every_minutes;
    int64_t candidate = 0;
    std::optional<Error> count_err;
    const auto count_at = [&](int64_t stride) -> int64_t {
        int64_t total = 0;
        for (const auto& inst : instruments) {
            auto count_res = pool.with_txn([&](pqxx::work& txn) {
                // One instrument's slice, with every aligned source LATERAL-joined.
            // 120s was sized for six talib-core sources; there are eleven now
            // (reference_lite for four venues and the cross-venue consensus),
            // and a slice cancelled halfway costs the whole cycle rather than
            // one instrument.
            txn.exec("SET LOCAL statement_timeout = '300s'");
                const auto row = txn.exec_params(
                    count_sql, inst.exchange_id, inst.instrument_id,
                    sets.feature_schema_hash, sets.label_set_id, date_from, date_to, stride);
                total += row[0][0].as<int64_t>();
            });
            if (!count_res) { count_err = count_res.error(); return 0; }
        }
        return total;
    };

    // Two bounds govern how many rows may be trained on, and coarsening has to
    // respect BOTH or it simply moves the failure downstream:
    //
    //   * `max_rows`        — a row-count ceiling, a policy about dataset size;
    //   * `max_matrix_mib`  — a MEMORY ceiling, a property of the deployment.
    //
    // The loop below used to check only `max_rows`, while the gate after it
    // checked only the memory bound. A dataset could therefore satisfy the loop
    // and still be refused, with no path back: as the archive grew past what the
    // memory budget allowed, the trainer stopped running entirely and said only
    // that the estimate exceeded the budget. Measured on this deployment it had
    // not completed a cycle since 08-09 while ingesting ~955k feature vectors.
    //
    // Coarsening is the right lever rather than a concession: the stride exists
    // to decorrelate adjacent 1m bars, so a coarser one trades sample count for
    // independence — it does not approximate the mathematics. Raising
    // `max_matrix_mib` on a larger host automatically pulls the stride back
    // toward the configured one, so the same code uses a bigger budget without
    // being retuned.
    // ---- point-in-time aligned sources ---------------------------------
    //
    // This is what replaced the old one-to-one query. That query joined the
    // Bitget 1m feature vector to its label on equal keys and stopped there, so
    // however much AltTech grew, the model saw one venue on one grid. Here the
    // target row still anchors the join, but every configured source is brought
    // in AS OF the target's close -- see aligned_source_sql() for the rule.
    std::vector<training::AlignedSource> sources;
    for (const auto& spec : cfg.t0_sources) {
        const auto src = training::parse_aligned_source(spec, sources.size());
        if (!src) return fail(ErrorCode::validation_error, "unusable t0_source: " + spec);
        sources.push_back(*src);
    }

    // Each source's columns, resolved against the layout of the set IT reads.
    // A name that is not in the layout is a configuration error, not something
    // to skip quietly: it would silently shrink the feature space.
    for (auto& src : sources) {
        std::vector<std::string> layout;
        if (src.feature_set == analytics::kCoreFeatureSetName &&
            src.feature_set_version == analytics::kCoreFeatureSetVersion) {
            layout = sets.feature_names;  // already resolved for the target
            src.equivalent_versions = {analytics::kCoreFeatureSetVersion};
        } else {
            auto loaded = load_set_layout(pool, src.feature_set, src.feature_set_version);
            if (!loaded) return std::unexpected(loaded.error());
            layout = std::move(loaded->columns);
            src.equivalent_versions = std::move(loaded->equivalent_versions);
        }
        if (!training::resolve_source_projection(src, layout, cfg.t0_projection,
                                                 analytics::kCoreFeatureSetName)) {
            return fail(ErrorCode::validation_error,
                        "t0_source projection unresolvable against " + src.feature_set + ":" +
                            std::to_string(src.feature_set_version) + " (" +
                            training::aligned_source_spec(src) + ")");
        }
    }

    // Resolved BEFORE the memory budget is computed, because the budget has to
    // be computed against the width the matrix will actually have. It was
    // computed against the published vector alone while the matrix also carried
    // the aligned block, so every cycle used more memory than the ceiling it had
    // just checked -- 7% more with six talib-core sources, 29% once the
    // reference_lite and cross-venue sets joined. A ceiling that is only checked
    // against part of the allocation is not a ceiling.
    data.n_features = static_cast<int32_t>(sets.feature_names.size() +
                                           training::aligned_block_width(sources));

    const int64_t memory_cap = training::memory_row_cap(cfg.max_matrix_mib, data.n_features);
    const int64_t row_cap =
        training::training_row_cap(cfg.max_rows, cfg.max_matrix_mib, data.n_features);

    // Bounded: each pass strictly increases the stride, so this terminates. The
    // ceiling is a day — beyond that the stride would be discarding so much
    // history that the honest answer is to report it, not to keep coarsening.
    constexpr int64_t kMaxStrideMinutes = 24 * 60;
    for (int attempt = 0; attempt < 8; ++attempt) {
        candidate = count_at(sample_minutes);
        if (count_err) return std::unexpected(*count_err);
        if (candidate <= row_cap) break;
        if (sample_minutes >= kMaxStrideMinutes) break;
        const int64_t scale = (candidate + row_cap - 1) / row_cap;
        sample_minutes = std::min(kMaxStrideMinutes,
                                  sample_minutes * std::max<int64_t>(scale, 2));
        log::info("training_sampling_scaled",
                  {{"candidate_rows", candidate},
                   {"sample_every_minutes", sample_minutes},
                   {"row_cap", row_cap},
                   {"max_rows", cfg.max_rows},
                   {"memory_row_cap", memory_cap}});
    }

    // Autonomy has to cut both ways. Coarsening the stride when the dataset would
    // overflow was implemented; REFINING it when the dataset is too small to
    // train on was not, so a configured stride of 5 discarded four fifths of the
    // history and the trainer then declined to run for want of rows it was
    // holding all along.
    //
    // The stride exists to decorrelate adjacent 1m bars, so it is relaxed as
    // little as possible: step down one minute at a time and keep the COARSEST
    // stride that clears min_rows. Two bounds are respected together, because
    // satisfying one alone just moves the failure: the row count must reach
    // min_rows AND the resulting matrix must fit max_matrix_mib. Halving the
    // stride instead of stepping it would jump 5 -> 2 and overshoot the memory
    // bound while a feasible stride of 3 or 4 sat between them.
    const int64_t max_feasible_rows = row_cap;
    if (candidate < cfg.min_rows && sample_minutes > 1) {
        int64_t best_stride = sample_minutes;
        int64_t best_rows = candidate;
        for (int64_t stride = sample_minutes - 1; stride >= 1; --stride) {
            const int64_t rows = count_at(stride);
            if (count_err) return std::unexpected(*count_err);
            if (rows > cfg.max_rows || rows > max_feasible_rows) break;   // finer strides only grow it
            best_stride = stride;
            best_rows = rows;
            if (rows >= cfg.min_rows) break;   // coarsest stride that clears the bar
        }
        if (best_stride != sample_minutes) {
            sample_minutes = best_stride;
            candidate = best_rows;
            log::info("training_sampling_refined",
                      {{"candidate_rows", candidate},
                       {"sample_every_minutes", sample_minutes},
                       {"min_rows", cfg.min_rows},
                       {"max_feasible_rows", max_feasible_rows}});
        }
        if (candidate < cfg.min_rows) {
            log::info("training_sampling_bounded",
                      {{"candidate_rows", candidate},
                       {"sample_every_minutes", sample_minutes},
                       {"min_rows", cfg.min_rows},
                       {"max_feasible_rows", max_feasible_rows},
                       {"n_features", static_cast<int64_t>(data.n_features)}});
        }
    }

    const int64_t matrix_bytes =
        candidate * static_cast<int64_t>(data.n_features) * 4;
    if (matrix_bytes * 4 > cfg.max_matrix_mib * 1024 * 1024) {
        // Reaching here means coarsening ran out of room — the stride hit its
        // ceiling and the dataset still does not fit. That is a capacity
        // statement about the host, so it names the numbers an operator needs
        // rather than only the setting that failed.
        log::error("training_matrix_budget_exhausted",
                   {{"candidate_rows", candidate},
                    {"n_features", static_cast<int64_t>(data.n_features)},
                    {"estimated_mib", (matrix_bytes * 4) / (1024 * 1024)},
                    {"max_matrix_mib", cfg.max_matrix_mib},
                    {"sample_every_minutes", sample_minutes},
                    {"row_cap", row_cap}});
        return fail(ErrorCode::validation_error,
                    "estimated training memory exceeds max_matrix_mib budget");
    }


    std::string source_joins;
    std::string source_cols;
    for (const auto& src : sources) {
        source_joins += training::aligned_source_sql(src);
        source_cols += ", " + src.alias + ".values::text";
    }

    const std::string load_sql =
        "SELECT (extract(epoch FROM f.bar_open_time) * 1000)::bigint, "
        "       f.values::text, f.validity_bitmap::text, "
        "       l.values::text, l.validity_bitmap::text" + source_cols + " "
        "FROM altml.feature_vectors f "
        "JOIN altml.labels l ON l.exchange_id = f.exchange_id "
        " AND l.instrument_id = f.instrument_id AND l.timeframe = f.timeframe "
        " AND l.bar_open_time = f.bar_open_time AND l.bar_revision = f.bar_revision " +
        quality_join_clause() + source_joins +
        quality_where_sql(sets.direction_pos, sets.return_pos) +
        "AND f.exchange_id = $1 AND f.instrument_id = $2 AND f.timeframe = '1m' "
        "AND f.schema_hash = $3 AND l.label_set_version_id = $4" + date_cond +
        "ORDER BY f.bar_open_time";

    const size_t n_feat = sets.feature_names.size();
    const size_t n_lab = sets.label_names.size();
    const size_t n_aligned = training::aligned_block_width(sources);

    // Provenance in the name itself: venue, grid, feature. A column whose
    // origin cannot be read off its name is a column nobody can audit later.
    for (auto& name : training::aligned_feature_names(sources)) {
        data.feature_names.push_back(std::move(name));
    }
    data.n_features = static_cast<int32_t>(n_feat + n_aligned);
    data.base_feature_count = static_cast<int32_t>(n_feat);
    // Stored in resolved form: whatever the config said, the model records the
    // sources it was actually built from, set and version included.
    data.aligned_sources.clear();
    for (const auto& src : sources) data.aligned_sources.push_back(training::aligned_source_spec(src));
    data.aligned_projection = cfg.t0_projection;
    data.X.reserve(static_cast<size_t>(candidate) * (n_feat + n_aligned));
    for (const auto& inst : instruments) {
        if (ShutdownToken::instance().stop_requested()) {
            return fail(ErrorCode::shutdown_requested, "shutdown during data load", true);
        }
        const auto sym_id = static_cast<int32_t>(data.symbols.size());
        size_t rows_for_symbol = 0;
        auto load_res = pool.with_txn([&](pqxx::work& txn) {
            // Same budget as the preflight count, for the same reason: the two
            // queries have the identical shape, one LATERAL per source, and a
            // slice cancelled halfway costs the whole cycle rather than one
            // instrument. At 120s the fetch fit fifteen sources with little to
            // spare -- measured, a sixteenth adds ~11s per instrument -- so the
            // ceiling that was already raised for the count is raised here.
            txn.exec("SET LOCAL statement_timeout = '300s'");
            for (const auto& row : txn.exec_params(
                     load_sql, inst.exchange_id, inst.instrument_id,
                     sets.feature_schema_hash, sets.label_set_id, date_from, date_to,
                     sample_minutes)) {
                const auto fvals = training::parse_pg_float_array(row[1].as<std::string>());
                const auto fbmp = training::parse_pg_hex_bytea(row[2].as<std::string>());
                const auto lvals = training::parse_pg_float_array(row[3].as<std::string>());
                const auto lbmp = training::parse_pg_hex_bytea(row[4].as<std::string>());
                if (fvals.size() != n_feat || lvals.size() != n_lab) continue;
                if (!training::bitmap_bit(lbmp, sets.direction_pos) ||
                    !training::bitmap_bit(lbmp, sets.return_pos)) {
                    continue; // target or return label not computable for this bar
                }
                const float dir = lvals[sets.direction_pos];
                const double r = static_cast<double>(lvals[sets.return_pos]);
                if (!std::isfinite(dir) || !std::isfinite(r)) continue;
                for (size_t i = 0; i < n_feat; ++i) {
                    data.X.push_back(training::bitmap_bit(fbmp, i)
                                         ? fvals[i]
                                         : std::numeric_limits<float>::quiet_NaN());
                }
                // Aligned sources, projected. A source that had no observation
                // at T0 -- venue down, grid still warming, observation too old
                // for its freshness bound -- contributes NaN, which LightGBM
                // treats as missing. It is never zero, never the previous
                // value, and never another venue's number: an absent
                // observation has to stay absent or the model learns from a
                // fact that did not exist.
                for (size_t si = 0; si < sources.size(); ++si) {
                    const auto& src = sources[si];
                    const auto& cell = row[static_cast<pqxx::row::size_type>(5 + si)];
                    if (cell.is_null()) {
                        for (size_t k = 0; k < src.positions.size(); ++k) {
                            data.X.push_back(std::numeric_limits<float>::quiet_NaN());
                        }
                        continue;
                    }
                    const auto svals = training::parse_pg_float_array(cell.as<std::string>());
                    for (const size_t pos : src.positions) {
                        data.X.push_back(pos < svals.size()
                                             ? svals[pos]
                                             : std::numeric_limits<float>::quiet_NaN());
                    }
                }
                data.y.push_back(dir > 0 ? 1 : (dir < 0 ? 0 : -1));
                data.r15.push_back(r);
                data.t_ms.push_back(row[0].as<int64_t>());
                data.sym_ids.push_back(sym_id);
                ++rows_for_symbol;
            }
        });
        if (!load_res) return std::unexpected(load_res.error());
        if (rows_for_symbol > 0) data.symbols.push_back(inst.symbol);
    }
    return data;
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
    double best_valid_auc = std::nan("");
    ~LgbmBooster() {
        if (handle != nullptr) LGBM_BoosterFree(handle);
    }
};

std::string lgbm_params(const TrainerConfig& cfg) {
    std::ostringstream out;
    out << "objective=binary metric=auc learning_rate=" << cfg.learning_rate
        << " num_leaves=" << cfg.num_leaves << " min_data_in_leaf=" << cfg.min_data_in_leaf
        << " feature_fraction=" << cfg.feature_fraction
        << " bagging_fraction=" << cfg.bagging_fraction
        << " bagging_freq=" << cfg.bagging_freq
        << " lambda_l1=" << cfg.lambda_l1 << " lambda_l2=" << cfg.lambda_l2
        << " max_depth=" << cfg.max_depth
        << " num_threads=" << cfg.threads << " verbosity=-1 seed=42 max_bin=255";
    return out.str();
}

// Gather selected rows into a contiguous row-major float buffer.
std::vector<float> gather_rows(const TrainingData& data, const std::vector<int64_t>& idx) {
    const auto n_feat = static_cast<size_t>(data.n_features);
    std::vector<float> out(idx.size() * n_feat);
    for (size_t r = 0; r < idx.size(); ++r) {
        const size_t src = static_cast<size_t>(idx[r]) * n_feat;
        std::copy_n(data.X.begin() + static_cast<std::ptrdiff_t>(src), n_feat,
                    out.begin() + static_cast<std::ptrdiff_t>(r * n_feat));
    }
    return out;
}

Result<void> make_dataset(const TrainingData& data, const std::vector<int64_t>& idx,
                          const std::string& params, DatasetHandle reference,
                          LgbmDataset& out) {
    const auto rows = gather_rows(data, idx);
    if (LGBM_DatasetCreateFromMat(rows.data(), C_API_DTYPE_FLOAT32,
                                  static_cast<int32_t>(idx.size()), data.n_features, 1,
                                  params.c_str(), reference, &out.handle) != 0) {
        return fail(ErrorCode::internal_error,
                    std::string("LGBM dataset failed: ") + LGBM_GetLastError());
    }
    std::vector<float> labels(idx.size());
    for (size_t i = 0; i < idx.size(); ++i) {
        labels[i] = static_cast<float>(data.y[static_cast<size_t>(idx[i])]);
    }
    if (LGBM_DatasetSetField(out.handle, "label", labels.data(),
                             static_cast<int>(labels.size()), C_API_DTYPE_FLOAT32) != 0) {
        return fail(ErrorCode::internal_error,
                    std::string("LGBM set label failed: ") + LGBM_GetLastError());
    }
    return {};
}

// Boosting with early stopping on validation AUC and a wall-clock budget.
Result<void> train_booster(const TrainingData& data, const std::vector<int64_t>& train_idx,
                           const std::vector<int64_t>& valid_idx, const TrainerConfig& cfg,
                           const std::chrono::steady_clock::time_point deadline,
                           int64_t fixed_iterations, LgbmBooster& out,
                           const std::string& ignore_columns = {}) {
    // `ignore_columns` carries the fold's own selection. It is applied to the
    // dataset rather than by reshaping the matrix, so the column indices stay
    // stable across folds and a feature dropped in one fold can return in the
    // next -- which is exactly what fold-local selection means.
    std::string params = lgbm_params(cfg);
    if (!ignore_columns.empty()) params += " ignore_column=" + ignore_columns;
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
        return fail(ErrorCode::internal_error,
                    std::string("LGBM booster failed: ") + LGBM_GetLastError());
    }
    if (with_valid && LGBM_BoosterAddValidData(out.handle, valid_set.handle) != 0) {
        return fail(ErrorCode::internal_error,
                    std::string("LGBM valid data failed: ") + LGBM_GetLastError());
    }

    const int64_t max_iter = fixed_iterations > 0 ? fixed_iterations : cfg.max_iterations;
    double best = -1.0;
    int best_iter = 0;
    int since_best = 0;
    for (int iter = 1; iter <= max_iter; ++iter) {
        if (ShutdownToken::instance().stop_requested()) {
            return fail(ErrorCode::shutdown_requested, "shutdown during boosting", true);
        }
        int finished = 0;
        if (LGBM_BoosterUpdateOneIter(out.handle, &finished) != 0) {
            return fail(ErrorCode::internal_error,
                        std::string("LGBM iteration failed: ") + LGBM_GetLastError());
        }
        if (with_valid) {
            int out_len = 0;
            double auc = 0;
            if (LGBM_BoosterGetEval(out.handle, 1, &out_len, &auc) != 0 || out_len < 1) {
                return fail(ErrorCode::internal_error,
                            std::string("LGBM eval failed: ") + LGBM_GetLastError());
            }
            if (auc > best) {
                best = auc;
                best_iter = iter;
                since_best = 0;
            } else if (++since_best >= cfg.early_stopping_rounds) {
                break;
            }
        } else {
            best_iter = iter;
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
    if (best_iter < 1) {
        return fail(ErrorCode::internal_error, "no boosting iterations completed");
    }
    out.best_iteration = best_iter;
    out.best_valid_auc = with_valid ? best : std::nan("");
    return {};
}

Result<std::vector<double>> predict_rows(BoosterHandle booster, const TrainingData& data,
                                         const std::vector<int64_t>& idx, int num_iteration) {
    const auto rows = gather_rows(data, idx);
    std::vector<double> out(idx.size());
    int64_t out_len = 0;
    if (LGBM_BoosterPredictForMat(booster, rows.data(), C_API_DTYPE_FLOAT32,
                                  static_cast<int32_t>(idx.size()), data.n_features, 1,
                                  C_API_PREDICT_NORMAL, 0, num_iteration, "", &out_len,
                                  out.data()) != 0 ||
        out_len != static_cast<int64_t>(idx.size())) {
        return fail(ErrorCode::internal_error,
                    std::string("LGBM predict failed: ") + LGBM_GetLastError());
    }
    return out;
}

// ------------------------------------------------------------- evaluation

std::vector<int64_t> directional_subset(const TrainingData& data,
                                        const std::vector<int64_t>& idx) {
    std::vector<int64_t> out;
    for (const int64_t i : idx) {
        if (data.y[static_cast<size_t>(i)] >= 0) out.push_back(i);
    }
    return out;
}

Result<void> require_both_classes(const TrainingData& data, const std::vector<int64_t>& idx,
                                  const std::string& context) {
    bool has_up = false, has_down = false;
    for (const int64_t i : idx) {
        if (data.y[static_cast<size_t>(i)] == 1) has_up = true;
        if (data.y[static_cast<size_t>(i)] == 0) has_down = true;
        if (has_up && has_down) return {};
    }
    return fail(ErrorCode::validation_error, context + ": needs both directional classes");
}

struct SliceEval {
    double auc = std::nan("");
    double logloss = std::nan("");
    double threshold = 0;
    training::EconomicResult econ;
};

// Threshold comes from the PRIOR slice predictions; economics run on the
// evaluation slice with all rows (flats stay in and pay costs).
SliceEval evaluate_slice(const TrainingData& data, const std::vector<int64_t>& eval_idx,
                         const std::vector<double>& eval_p, double threshold,
                         const TrainerConfig& cfg) {
    SliceEval out;
    out.threshold = threshold;
    std::vector<int> y_dir;
    std::vector<double> p_dir;
    std::vector<int64_t> times;
    std::vector<int32_t> syms;
    std::vector<double> rets;
    times.reserve(eval_idx.size());
    for (size_t k = 0; k < eval_idx.size(); ++k) {
        const auto i = static_cast<size_t>(eval_idx[k]);
        if (data.y[i] >= 0) {
            y_dir.push_back(data.y[i]);
            p_dir.push_back(eval_p[k]);
        }
        times.push_back(data.t_ms[i]);
        syms.push_back(data.sym_ids[i]);
        rets.push_back(data.r15[i]);
    }
    out.auc = training::roc_auc(y_dir, p_dir);
    out.logloss = training::log_loss(y_dir, p_dir);
    const auto selected = training::select_non_overlapping(eval_p, times, syms, threshold,
                                                           kHoldingMs);
    out.econ = training::evaluate_directional(selected, eval_p, rets, cfg.round_trip_fee_bps,
                                              cfg.round_trip_slippage_bps);
    return out;
}

// ------------------------------------------------------------- registry

struct RegistryRefs {
    std::string model_id;
    std::string model_dir;
    std::string model_path;
    std::string model_checksum;
};

std::string file_sha256(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return sha256_hex(buffer.str());
}

std::string random_hex8() {
    std::random_device rd;
    std::uniform_int_distribution<uint32_t> dist;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", dist(rd));
    return buf;
}

json fold_metrics_json(const SliceEval& eval) {
    return json{{"roc_auc", eval.auc},
                {"logloss", eval.logloss},
                {"conviction_threshold", eval.threshold},
                {"trades", eval.econ.trades},
                {"expected_return_after_costs", eval.econ.expectancy_after_costs},
                {"profit_factor", eval.econ.profit_factor},
                {"hit_rate", eval.econ.hit_rate},
                {"max_drawdown", eval.econ.max_drawdown},
                {"total_return", eval.econ.total_return},
                {"long_share", eval.econ.long_share}};
}

// ------------------------------------------------------------- cycle outcome

struct CycleOutcome {
    std::string model_id;
    std::string status;   // ACTIVE | VALIDATED | REJECTED
    std::string detail;
};

// See kDefaultTargetLabel: the registry must describe the label actually trained.
std::string objective_of(const TrainerConfig& cfg) { return "binary:y_" + cfg.target_label; }

Result<CycleOutcome> run_cycle(persistence::PgPool& pool, const config::RuntimeConfig& rt,
                               const TrainerConfig& cfg) {
    const auto started = std::chrono::steady_clock::now();
    const std::string objective = objective_of(cfg);

    auto sets = resolve_sets(pool, cfg);
    if (!sets) return std::unexpected(sets.error());

    auto data_res = load_training_data(pool, *sets, cfg, rt.project.exchange);
    if (!data_res) return std::unexpected(data_res.error());
    TrainingData& data = *data_res;

    // The budget belongs to BOOSTING, and is therefore measured from the moment
    // boosting can start.
    //
    // It used to run from the top of the cycle, so every second spent loading
    // the matrix came out of it. Measured on VPS1: the load took 46 minutes
    // against a 40-minute budget, so boosting was handed a deadline that had
    // already passed and the cycle ended with "time budget exhausted before
    // minimum iterations" -- with the folds selected, the matrix in memory and
    // not one tree grown. The model registry stayed empty while every stage
    // before boosting reported success.
    //
    // Loading is bounded by the query itself and by row_cap, not by this
    // number; what this number bounds is how long the trees may take.
    const auto load_finished = std::chrono::steady_clock::now();
    const auto deadline = load_finished + std::chrono::seconds(cfg.time_budget_sec);
    log::info("training_data_loaded",
              {{"load_seconds",
                std::chrono::duration_cast<std::chrono::seconds>(load_finished - started).count()},
               {"boosting_budget_sec", cfg.time_budget_sec}});
    if (static_cast<int64_t>(data.rows()) < cfg.min_rows) {
        return fail(ErrorCode::validation_error,
                    "insufficient labeled history: rows=" + std::to_string(data.rows()) +
                        " min_rows=" + std::to_string(cfg.min_rows),
                    true);
    }
    log::info("training_started",
              {{"rows", static_cast<int64_t>(data.rows())},
               {"features", data.n_features},
               {"sample_every_minutes", data.effective_sample_minutes}});

    const int64_t embargo_ms = cfg.embargo_minutes * 60'000;
    std::vector<training::FoldIdx> folds;
    training::FinalSplitIdx final_split;
    try {
        folds = training::build_walk_forward_folds(data.t_ms,
                                                   static_cast<size_t>(cfg.folds), embargo_ms);
        final_split = training::build_final_split(data.t_ms, static_cast<size_t>(cfg.folds),
                                                  embargo_ms, cfg.validation_fraction);
    } catch (const std::exception& e) {
        return fail(ErrorCode::validation_error, std::string("temporal split: ") + e.what());
    }

    // Walk-forward folds: metrics come only from each fold's future test slice.
    std::vector<SliceEval> fold_evals;
    for (size_t k = 0; k < folds.size(); ++k) {
        const auto tr_fit = directional_subset(data, folds[k].train);
        const auto va_fit = directional_subset(data, folds[k].valid);
        const std::string ctx = "fold " + std::to_string(k);
        for (const auto* pair : {&tr_fit, &va_fit}) {
            if (auto r = require_both_classes(data, *pair, ctx); !r) {
                return std::unexpected(r.error());
            }
        }
        // Fold-local feature selection: fitted on THIS fold's training rows and
        // nothing else. Running it once over the whole dataset would let every
        // fold's filter see the future it is about to be scored on.
        const auto sel = training::select_features_fold_local(
            data.X, static_cast<size_t>(data.n_features), folds[k].train);
        std::string ignore;
        std::map<std::string, int64_t> reject_counts;
        for (size_t f = 0; f < sel.reason.size(); ++f) {
            if (sel.reason[f] == training::RejectReason::kept) continue;
            if (!ignore.empty()) ignore += ',';
            ignore += std::to_string(f);
            ++reject_counts[training::reject_reason_name(sel.reason[f])];
        }
        {
            json reasons = json::object();
            for (const auto& [why, n] : reject_counts) reasons[why] = n;
            log::info("training_fold_selection",
                      {{"fold", static_cast<int64_t>(k)},
                       {"selection_version", static_cast<int64_t>(training::kSelectionVersion)},
                       {"train_rows", static_cast<int64_t>(folds[k].train.size())},
                       {"features_in", static_cast<int64_t>(data.n_features)},
                       {"selected", static_cast<int64_t>(sel.selected.size())},
                       {"rejected", static_cast<int64_t>(sel.rejected())},
                       {"reasons", reasons}});
        }

        LgbmBooster booster;
        if (auto r = train_booster(data, tr_fit, va_fit, cfg, deadline, 0, booster, ignore); !r) {
            return std::unexpected(r.error());
        }
        auto pva = predict_rows(booster.handle, data, folds[k].valid, booster.best_iteration);
        if (!pva) return std::unexpected(pva.error());
        auto pte = predict_rows(booster.handle, data, folds[k].test, booster.best_iteration);
        if (!pte) return std::unexpected(pte.error());
        const double threshold =
            training::conviction_threshold(*pva, cfg.conviction_quantile);
        auto eval = evaluate_slice(data, folds[k].test, *pte, threshold, cfg);
        if (eval.econ.trades < cfg.min_trades) {
            return fail(ErrorCode::validation_error,
                        ctx + " has only " + std::to_string(eval.econ.trades) +
                            " non-overlapping trades");
        }
        log::info("training_fold_done",
                  {{"fold", static_cast<int64_t>(k)},
                   {"auc", eval.auc},
                   {"expectancy_after_costs", eval.econ.expectancy_after_costs},
                   {"trades", eval.econ.trades}});
        fold_evals.push_back(std::move(eval));
    }

    // Final: tune on the purged train/validation split, then refit on every
    // leakage-safe row; the held-out test is touched exactly once.
    const auto final_train_fit = directional_subset(data, final_split.train);
    const auto final_valid_fit = directional_subset(data, final_split.valid);
    const auto final_refit = directional_subset(data, final_split.fit);
    for (const auto& [idx, ctx] :
         {std::pair{&final_train_fit, "final train"}, {&final_valid_fit, "final valid"},
          {&final_refit, "final refit"}}) {
        if (auto r = require_both_classes(data, *idx, ctx); !r) {
            return std::unexpected(r.error());
        }
    }
    LgbmBooster tuned;
    if (auto r = train_booster(data, final_train_fit, final_valid_fit, cfg, deadline, 0, tuned);
        !r) {
        return std::unexpected(r.error());
    }
    auto pcal = predict_rows(tuned.handle, data, final_split.valid, tuned.best_iteration);
    if (!pcal) return std::unexpected(pcal.error());
    const double final_threshold =
        training::conviction_threshold(*pcal, cfg.conviction_quantile);

    LgbmBooster final_model;
    if (auto r = train_booster(data, final_refit, {}, cfg, deadline, tuned.best_iteration,
                               final_model);
        !r) {
        return std::unexpected(r.error());
    }
    auto pte = predict_rows(final_model.handle, data, final_split.test,
                            final_model.best_iteration);
    if (!pte) return std::unexpected(pte.error());
    const auto final_eval = evaluate_slice(data, final_split.test, *pte, final_threshold, cfg);

    // ------------------------------------------------ promotion gates (hard)
    std::vector<std::string> gate_failures;
    double mean_auc = 0, mean_expectancy = 0;
    int64_t positive_folds = 0;
    for (const auto& e : fold_evals) {
        mean_auc += e.auc;
        mean_expectancy += e.econ.expectancy_after_costs;
        if (e.econ.expectancy_after_costs > 0) ++positive_folds;
    }
    mean_auc /= static_cast<double>(fold_evals.size());
    mean_expectancy /= static_cast<double>(fold_evals.size());
    if (!(mean_auc >= cfg.min_mean_fold_auc)) {
        gate_failures.push_back("mean fold AUC " + std::to_string(mean_auc) + " < min");
    }
    if (!(mean_expectancy > 0)) {
        gate_failures.push_back("mean fold expectancy <= 0");
    }
    if (positive_folds < cfg.min_positive_folds) {
        gate_failures.push_back("positive folds " + std::to_string(positive_folds) + "/" +
                                std::to_string(fold_evals.size()));
    }
    if (!(final_eval.econ.expectancy_after_costs > 0)) {
        gate_failures.push_back("final test expectancy <= 0");
    }
    if (!(final_eval.econ.total_return > 0)) {
        gate_failures.push_back("final test total return <= 0");
    }
    if (!(final_eval.econ.profit_factor >= cfg.min_profit_factor)) {
        gate_failures.push_back("final profit factor below minimum");
    }
    if (!(final_eval.econ.max_drawdown <= cfg.max_drawdown)) {
        gate_failures.push_back("final drawdown above maximum");
    }
    if (final_eval.econ.trades < cfg.min_trades) {
        gate_failures.push_back("final trades below minimum");
    }

    // ------------------------------------- champion-challenger on same holdout
    std::string champion_id;
    double champion_expectancy = std::nan("");
    {
        std::string champ_path, champ_checksum;
        int champ_features = 0;
        auto champ_res = pool.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec_params(
                "SELECT m.model_id, a.path, a.checksum, c.feature_count FROM altml.models m "
                "JOIN altml.model_artifacts a ON a.model_id = m.model_id AND a.kind = 'model' "
                "JOIN altml.model_feature_compatibility c ON c.model_id = m.model_id "
                "WHERE m.status = 'ACTIVE' AND m.algorithm = 'lightgbm' "
                "AND m.objective = $1 LIMIT 1",
                objective);
            if (!rows.empty()) {
                champion_id = rows[0][0].as<std::string>();
                champ_path = rows[0][1].as<std::string>();
                champ_checksum = rows[0][2].as<std::string>();
                champ_features = rows[0][3].as<int>();
            }
        });
        if (!champ_res) return std::unexpected(champ_res.error());
        if (!champion_id.empty() && champ_features == data.n_features &&
            file_sha256(champ_path) == champ_checksum) {
            BoosterHandle champ = nullptr;
            int champ_iters = 0;
            if (LGBM_BoosterCreateFromModelfile(champ_path.c_str(), &champ_iters, &champ) == 0) {
                auto champ_cal = predict_rows(champ, data, final_split.valid, -1);
                auto champ_te = predict_rows(champ, data, final_split.test, -1);
                if (champ_cal && champ_te) {
                    const double champ_threshold = training::conviction_threshold(
                        *champ_cal, cfg.conviction_quantile);
                    const auto champ_eval = evaluate_slice(data, final_split.test, *champ_te,
                                                           champ_threshold, cfg);
                    champion_expectancy = champ_eval.econ.expectancy_after_costs;
                }
                LGBM_BoosterFree(champ);
            }
        } else if (!champion_id.empty()) {
            log::warn("champion_not_comparable", {{"model_id", champion_id}});
            champion_id.clear();
        }
    }
    if (!champion_id.empty() && std::isfinite(champion_expectancy) &&
        !(final_eval.econ.expectancy_after_costs > champion_expectancy)) {
        gate_failures.push_back("challenger does not beat champion on shared holdout");
    }

    // ------------------------------------------------------------- artifacts
    const std::string stamp = [] {
        const int64_t ms = timeutil::now_ms();
        std::string iso = timeutil::to_iso8601(ms); // 2026-07-25T10:00:00.000Z
        std::string out;
        for (const char ch : iso) {
            if (ch == '-' || ch == ':') continue;
            if (ch == '.') break;
            out += ch;
        }
        return out + "Z";
    }();
    RegistryRefs refs;
    refs.model_id = stamp + "-lightgbm-" + random_hex8();
    const fs::path model_root(rt.project.model_root);
    const fs::path tmp_dir = model_root / (".tmp-" + refs.model_id);
    const fs::path final_dir = model_root / refs.model_id;
    std::error_code fs_error;
    fs::create_directories(tmp_dir, fs_error);
    if (fs_error) {
        return fail(ErrorCode::internal_error,
                    "cannot create model directory: " + fs_error.message());
    }
    const std::string model_path = (tmp_dir / "model.txt").string();
    if (LGBM_BoosterSaveModel(final_model.handle, 0, final_model.best_iteration,
                              C_API_FEATURE_IMPORTANCE_SPLIT, model_path.c_str()) != 0) {
        return fail(ErrorCode::internal_error,
                    std::string("LGBM save failed: ") + LGBM_GetLastError());
    }

    // Write/read parity self-check: the artifact that will serve live must
    // reproduce the in-memory predictions on the sample rows.
    std::vector<int64_t> sample_idx(final_split.test.begin(),
                                    final_split.test.begin() +
                                        std::min<std::ptrdiff_t>(kSampleRows,
                                                                 static_cast<std::ptrdiff_t>(
                                                                     final_split.test.size())));
    BoosterHandle reloaded = nullptr;
    int reloaded_iters = 0;
    if (LGBM_BoosterCreateFromModelfile(model_path.c_str(), &reloaded_iters, &reloaded) != 0) {
        return fail(ErrorCode::internal_error,
                    std::string("saved model does not load: ") + LGBM_GetLastError());
    }
    auto reloaded_p = predict_rows(reloaded, data, sample_idx, -1);
    LGBM_BoosterFree(reloaded);
    if (!reloaded_p) return std::unexpected(reloaded_p.error());
    for (size_t i = 0; i < sample_idx.size(); ++i) {
        const double in_memory = (*pte)[i];
        if (std::abs((*reloaded_p)[i] - in_memory) > kSelfCheckTolerance) {
            return fail(ErrorCode::validation_error,
                        "write/read parity failed at sample row " + std::to_string(i));
        }
    }

    json feature_names_json = json::array();
    for (const auto& n : data.feature_names) feature_names_json.push_back("f_" + n);
    json sample = {{"rows", json::array()}, {"expected", json::array()}};
    const auto n_feat = static_cast<size_t>(data.n_features);
    for (size_t i = 0; i < sample_idx.size(); ++i) {
        json row = json::array();
        const size_t base = static_cast<size_t>(sample_idx[i]) * n_feat;
        for (size_t f = 0; f < n_feat; ++f) {
            const float v = data.X[base + f];
            if (std::isfinite(v)) row.push_back(v); else row.push_back(nullptr);
        }
        sample["rows"].push_back(std::move(row));
        sample["expected"].push_back((*reloaded_p)[i]);
    }
    json folds_json = json::array();
    for (size_t k = 0; k < fold_evals.size(); ++k) {
        folds_json.push_back({{"fold", k}, {"metrics", fold_metrics_json(fold_evals[k])}});
    }
    const bool gates_passed = gate_failures.empty();
    json report = {{"trainer_version", kTrainerVersion},
                   {"objective", objective},
                   {"rows", data.rows()},
                   {"features", data.n_features},
                   {"sample_every_minutes", data.effective_sample_minutes},
                   {"walk_forward", folds_json},
                   {"mean_fold_auc", mean_auc},
                   {"mean_fold_expectancy", mean_expectancy},
                   {"positive_folds", positive_folds},
                   {"final_test", fold_metrics_json(final_eval)},
                   {"best_iterations", final_model.best_iteration},
                   {"champion", champion_id.empty() ? json(nullptr) : json(champion_id)},
                   {"champion_expectancy", std::isfinite(champion_expectancy)
                                               ? json(champion_expectancy)
                                               : json(nullptr)},
                   {"gates_passed", gates_passed},
                   {"gate_failures", gate_failures},
                   {"protocol",
                    {{"timestamp_grouped", true},
                     {"embargo_minutes", cfg.embargo_minutes},
                     {"walk_forward_metrics_scope", "test"},
                     {"confidence_calibration_source", "prior_validation"},
                     {"flat_label_policy", "economic_test_only"},
                     {"directional_long_short", true},
                     {"overlapping_positions", false},
                     {"round_trip_fee_bps", cfg.round_trip_fee_bps},
                     {"round_trip_slippage_bps", cfg.round_trip_slippage_bps}}}};

    const auto write_json = [&](const char* file, const json& value) {
        std::ofstream out(tmp_dir / file);
        out << value.dump(1);
        return out.good();
    };
    if (!write_json("feature_names.json", feature_names_json) ||
        !write_json("sample_predictions.json", sample) || !write_json("report.json", report)) {
        return fail(ErrorCode::internal_error, "cannot write model artifacts");
    }
    fs::rename(tmp_dir, final_dir, fs_error); // atomic publish; dir is immutable now
    if (fs_error) {
        return fail(ErrorCode::internal_error, "cannot publish model dir: " + fs_error.message());
    }
    refs.model_dir = final_dir.string();
    refs.model_path = (final_dir / "model.txt").string();
    refs.model_checksum = file_sha256(refs.model_path);

    // ------------------------------------------------------------- registry
    const int64_t t_lo = *std::min_element(data.t_ms.begin(), data.t_ms.end());
    const int64_t t_hi = *std::max_element(data.t_ms.begin(), data.t_ms.end()) + 60'000;
    const std::string status_after_gates =
        gates_passed ? (cfg.auto_promote ? "ACTIVE" : "APPROVED") : "REJECTED";
    auto reg = pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altml.models (model_id, algorithm, objective, feature_set_version, "
            "label_set_version, dataset_id, hyperparameters, status, training_period) "
            "VALUES ($1, 'lightgbm', $2, $3, 'core-labels:1', $4, $5, 'CANDIDATE', "
            "tstzrange(to_timestamp($6::bigint / 1000.0), to_timestamp($7::bigint / 1000.0), "
            "'[)'))",
            refs.model_id, objective,
            std::string(analytics::kCoreFeatureSetName) + ":" +
                std::to_string(analytics::kCoreFeatureSetVersion),
            "native-" + stamp,
            json({{"learning_rate", cfg.learning_rate},
                  {"num_leaves", cfg.num_leaves},
                  {"min_data_in_leaf", cfg.min_data_in_leaf},
                  {"feature_fraction", cfg.feature_fraction},
                  {"bagging_fraction", cfg.bagging_fraction},
                  {"lambda_l1", cfg.lambda_l1},
                  {"lambda_l2", cfg.lambda_l2},
                  {"max_depth", cfg.max_depth},
                  {"selected_iterations", final_model.best_iteration},
                  {"conviction_threshold", final_threshold},
                  {"trainer_version", kTrainerVersion}})
                .dump(),
            t_lo, t_hi);
        for (const auto& [kind, file, format] :
             {std::tuple{"model", "model.txt", "lightgbm-text"},
              {"feature_names", "feature_names.json", "json"},
              {"sample_predictions", "sample_predictions.json", "json"},
              {"report", "report.json", "json"}}) {
            const std::string path = (final_dir / file).string();
            txn.exec_params(
                "INSERT INTO altml.model_artifacts (model_id, kind, path, format, checksum, "
                "size_bytes) VALUES ($1, $2, $3, $4, $5, $6)",
                refs.model_id, std::string(kind), path, std::string(format), file_sha256(path),
                static_cast<int64_t>(fs::file_size(path)));
        }
        const auto insert_metric = [&](const std::string& scope, const std::string& metric,
                                       double value) {
            if (!std::isfinite(value)) return;
            txn.exec_params(
                "INSERT INTO altml.model_metrics (model_id, scope, metric, value) "
                "VALUES ($1, $2, $3, $4) ON CONFLICT DO NOTHING",
                refs.model_id, scope, metric, value);
        };
        insert_metric("validation", "mean_fold_auc", mean_auc);
        insert_metric("validation", "mean_expected_return_after_costs", mean_expectancy);
        insert_metric("validation", "positive_folds", static_cast<double>(positive_folds));
        insert_metric("test", "roc_auc", final_eval.auc);
        insert_metric("test", "logloss", final_eval.logloss);
        insert_metric("test", "expected_return_after_costs",
                      final_eval.econ.expectancy_after_costs);
        insert_metric("test", "profit_factor", final_eval.econ.profit_factor);
        insert_metric("test", "max_drawdown", final_eval.econ.max_drawdown);
        insert_metric("test", "total_return", final_eval.econ.total_return);
        insert_metric("test", "trades", static_cast<double>(final_eval.econ.trades));
        for (size_t k = 0; k < fold_evals.size(); ++k) {
            const auto& fold = folds[k];
            const auto& e = fold_evals[k];
            txn.exec_params(
                "INSERT INTO altml.model_walk_forward_runs (model_id, fold, train_from, train_to, "
                "valid_from, valid_to, test_from, test_to, metrics) VALUES ($1, $2, "
                "to_timestamp($3::bigint / 1000.0), to_timestamp($4::bigint / 1000.0), "
                "to_timestamp($5::bigint / 1000.0), to_timestamp($6::bigint / 1000.0), "
                "to_timestamp($7::bigint / 1000.0), to_timestamp($8::bigint / 1000.0), $9)",
                refs.model_id, static_cast<int>(k),
                data.t_ms[static_cast<size_t>(fold.train.front())],
                data.t_ms[static_cast<size_t>(fold.train.back())] + 60'000,
                data.t_ms[static_cast<size_t>(fold.valid.front())],
                data.t_ms[static_cast<size_t>(fold.valid.back())] + 60'000,
                data.t_ms[static_cast<size_t>(fold.test.front())],
                data.t_ms[static_cast<size_t>(fold.test.back())] + 60'000,
                fold_metrics_json(e).dump());
        }
        txn.exec_params(
            "INSERT INTO altml.model_feature_compatibility (model_id, feature_set_version, "
            "schema_hash, feature_count, base_feature_count, aligned_sources, "
            "aligned_projection) VALUES ($1, $2, $3, $4, $5, $6, $7)",
            refs.model_id,
            std::string(analytics::kCoreFeatureSetName) + ":" +
                std::to_string(analytics::kCoreFeatureSetVersion),
            data.feature_schema_hash, data.n_features, data.base_feature_count,
            training::pg_text_array_literal(data.aligned_sources),
            training::pg_text_array_literal(data.aligned_projection));
        txn.exec_params(
            "INSERT INTO altml.model_events (model_id, event, detail, actor) "
            "VALUES ($1, 'trained', $2, 'altml-trainer')",
            refs.model_id,
            json({{"gates_passed", gates_passed}, {"gate_failures", gate_failures}}).dump());
    });
    if (!reg) return std::unexpected(reg.error());

    CycleOutcome outcome;
    outcome.model_id = refs.model_id;
    if (!gates_passed) {
        auto rej = pool.with_txn([&](pqxx::work& txn) {
            txn.exec_params("UPDATE altml.models SET status = 'REJECTED' WHERE model_id = $1",
                            refs.model_id);
            txn.exec_params(
                "INSERT INTO altml.model_events (model_id, event, detail, actor) "
                "VALUES ($1, 'rejected', $2, 'altml-trainer')",
                refs.model_id, json({{"gate_failures", gate_failures}}).dump());
        });
        if (!rej) return std::unexpected(rej.error());
        outcome.status = "REJECTED";
        outcome.detail = gate_failures.front();
        return outcome;
    }
    if (!cfg.auto_promote) {
        auto appr = pool.with_txn([&](pqxx::work& txn) {
            txn.exec_params(
                "UPDATE altml.models SET status = 'APPROVED', approved_at = now() "
                "WHERE model_id = $1",
                refs.model_id);
        });
        if (!appr) return std::unexpected(appr.error());
        outcome.status = "APPROVED";
        outcome.detail = "gates passed; waiting for operator activation";
        return outcome;
    }

    // Transactional champion switch (port of the retired promote step): lock
    // the candidate and the current ACTIVE row, retire, activate, audit.
    auto promo = pool.with_txn([&](pqxx::work& txn) {
        const auto locked = txn.exec_params(
            "SELECT status FROM altml.models WHERE model_id = $1 FOR UPDATE", refs.model_id);
        if (locked.empty() || locked[0][0].as<std::string>() != "CANDIDATE") {
            throw std::runtime_error("candidate status changed during promotion");
        }
        const auto prev = txn.exec_params(
            "SELECT model_id FROM altml.models WHERE status = 'ACTIVE' AND objective = $1 "
            "FOR UPDATE",
            objective);
        std::string prev_id;
        if (!prev.empty()) {
            prev_id = prev[0][0].as<std::string>();
            txn.exec_params(
                "UPDATE altml.models SET status = 'RETIRED', retired_at = now() "
                "WHERE model_id = $1",
                prev_id);
            txn.exec_params(
                "INSERT INTO altml.model_events (model_id, event, detail, actor) "
                "VALUES ($1, 'retired', $2, 'altml-trainer')",
                prev_id, json({{"replaced_by", refs.model_id}}).dump());
        }
        txn.exec_params(
            "UPDATE altml.models SET status = 'ACTIVE', approved_at = now(), "
            "activated_at = now(), retired_at = NULL WHERE model_id = $1",
            refs.model_id);
        txn.exec_params(
            "INSERT INTO altml.model_events (model_id, event, detail, actor) "
            "VALUES ($1, 'activated', $2, 'altml-trainer')",
            refs.model_id,
            json({{"previous", prev_id}, {"checksum", refs.model_checksum}}).dump());
    });
    if (!promo) return std::unexpected(promo.error());
    outcome.status = "ACTIVE";
    outcome.detail = "promoted; previous champion retired";
    return outcome;
}

// ====================================================================== econ
//
// The ML-first trainer: one LightGBM multiclass model per horizon (15m, 1h,
// 4h) on the econ-labels target (SHORT / NEUTRAL / LONG against the all-in
// cost), temperature-calibrated on a validation slice that precedes every
// slice it is judged on, and gated on proper scores and on the economics of
// acting on its expected net return. See econ_logic.hpp for the arithmetic
// and feature_recipe.hpp for the feature space.
//
// Point in time. Every aligned observation is admitted only if AltTech had
// COMPUTED it by the time the target's own vector was computed
// (`computed_at <= f.computed_at`), not merely if its bar had closed. The
// legacy join used bar close alone, and on VPS1 the 15m and 1h vectors are
// computed a median 176 s and 254 s after their bar closes (p90 593 s and
// 2524 s): the model was trained on a 15m bar that the live service, scoring
// the same minute, did not yet have. A backfilled row carries the time it was
// actually computed, so history the live stream never delivered stays out.
//
// Validity. A source slot whose validity bit is clear is NaN in the matrix.
// The legacy loader read source values without their bitmap, so an invalid
// slot entered training as its stored placeholder -- 0 -- while inference
// (which did read the bitmap) served NaN for the same slot.

constexpr const char* kEconTrainerVersion = "native-econ-2.0.0";

struct EconLabelInfo {
    int64_t set_id = 0;
    int fee_model_version = 0;
    size_t width = 0;
    // per horizon index (econ::kHorizons order): class, return, mfe, mae
    std::array<std::array<size_t, 4>, 3> pos{};
};

Result<EconLabelInfo> resolve_econ_labels(persistence::PgPool& pool) {
    EconLabelInfo info;
    std::vector<std::string> names;
    std::string error;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto ls = txn.exec_params(
            "SELECT id, frozen, fee_model_version FROM altml.label_set_versions "
            "WHERE name = 'econ-labels' AND version = 1");
        if (ls.empty() || !ls[0][1].as<bool>()) {
            error = "label set econ-labels:1 missing or not frozen";
            return;
        }
        info.set_id = ls[0][0].as<int64_t>();
        info.fee_model_version = ls[0][2].as<int>();
        for (const auto& row : txn.exec_params(
                 "SELECT output_name FROM altml.label_set_members "
                 "WHERE label_set_version_id = $1 ORDER BY position",
                 info.set_id)) {
            names.push_back(row[0].as<std::string>());
        }
    });
    if (!res) return std::unexpected(res.error());
    if (!error.empty()) return fail(ErrorCode::validation_error, error);
    info.width = names.size();
    for (size_t h = 0; h < econ::kHorizons.size(); ++h) {
        const std::string n(econ::kHorizons[h].name);
        const std::array<std::string, 4> want = {"econ_class_" + n, "log_return_" + n,
                                                 "mfe_long_" + n, "mae_long_" + n};
        for (size_t k = 0; k < 4; ++k) {
            const auto it = std::find(names.begin(), names.end(), want[k]);
            if (it == names.end()) {
                return fail(ErrorCode::validation_error, "econ-labels lacks " + want[k]);
            }
            info.pos[h][k] = static_cast<size_t>(it - names.begin());
        }
    }
    return info;
}

// One aligned source as the econ recipe reads it.
struct EconSource {
    training::AlignedSource src;   // projection/positions = the columns taken
    bool derived = false;          // talib-core sources also get the derived block
    recipe::DerivedRecipe recipe;  // positions in the source's own full layout
};

struct EconData {
    std::vector<float> X;
    int32_t n_features = 0;
    std::vector<std::string> feature_names;
    std::string feature_schema_hash;
    std::vector<int64_t> t_ms;
    std::vector<int32_t> sym_ids;
    std::vector<std::string> symbols;
    std::array<std::vector<int>, 3> y;            // -1 = no outcome for that horizon
    std::array<std::vector<double>, 3> r, mfe, mae;
    // Cost model v2 at each row's T0: the round trip as a fraction, and the
    // spread it was built from (observed, or the stated fallback).
    std::vector<double> cost;
    std::vector<float> spread_bps;
    std::vector<uint8_t> spread_observed;
    int64_t sample_minutes = 0;
    int32_t base_width = 0;
    std::vector<std::string> base_projection;
    std::vector<std::string> source_specs;
    [[nodiscard]] size_t rows() const { return t_ms.size(); }
};

// Full vector with invalid slots as NaN.
std::vector<float> with_validity(const std::vector<float>& values,
                                 const std::vector<uint8_t>& bitmap) {
    std::vector<float> out(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        out[i] = training::bitmap_bit(bitmap, i) ? values[i]
                                                 : std::numeric_limits<float>::quiet_NaN();
    }
    return out;
}

// Where the Bitget book spread sits in each frozen bitget_micro version.
Result<std::map<int, int>> spread_positions(persistence::PgPool& pool) {
    std::map<int, int> spread_pos;   // version -> 1-based array position
    auto r = pool.with_txn([&](pqxx::work& txn) {
        for (const auto& row : txn.exec(
                 "SELECT v.version, m.position FROM altml.fdw_feature_set_versions v "
                 "JOIN altml.fdw_feature_set_members m ON m.feature_set_version_id = v.id "
                 "WHERE v.name = 'bitget_micro' AND v.frozen AND m.output_name = 'book_spread_bps'")) {
            spread_pos[row[0].as<int>()] = row[1].as<int>() + 1;
        }
    });
    if (!r) return std::unexpected(r.error());
    return spread_pos;
}

// The spread observed at T0 of target row `f`, as column `cs.spread`: the
// latest VALID bitget_micro window of this instrument that was AVAILABLE when
// the target vector was, at most five minutes old, with its validity bit set.
// The trainer's cost and replay's archive check read it through this one
// statement.
//
// "Available" is arrival where both rows arrived live. The micro window of
// minute T-1 and the target vector of minute T are computed about a second
// apart (p10/p50/p90 of the difference: -2.1/-1.0/+0.1 s), so ordering them by
// computed_at settled a race the live process never saw: inference prices a
// target with what has ARRIVED, and on 21% of rows (314 of 1500, replay of
// econ1h 2026-09-25) the T-1 window had been computed first but arrived
// second. The trainer then charged a spread live could not have known. Ordered
// by received_at, the archive reproduces the live cost input 1500/1500. A
// backfilled row's received_at is the backfill's clock, not availability, so
// there computed_at remains the proxy.
std::string spread_lateral_sql(const std::map<int, int>& spread_pos) {
    std::string spread_case = "NULL::float8";
    if (!spread_pos.empty()) {
        spread_case = "CASE cs_i.feature_set_version";
        for (const auto& [ver, pos] : spread_pos) {
            spread_case += " WHEN " + std::to_string(ver) + " THEN CASE WHEN get_bit(cs_i.validity_bitmap, " +
                           std::to_string(pos - 1) + ") = 1 THEN cs_i.values[" + std::to_string(pos) +
                           "]::float8 END";
        }
        spread_case += " END";
    }
    return "LEFT JOIN LATERAL (SELECT " + spread_case + " AS spread FROM altml.econ_feature_vectors cs_i "
           "WHERE cs_i.exchange_id = f.exchange_id AND cs_i.instrument_id = f.instrument_id "
           "AND cs_i.timeframe = '1m' AND cs_i.feature_set_name = 'bitget_micro' "
           "AND cs_i.quality_status = 'VALID' AND cs_i.bar_open_time <= f.bar_open_time "
           "AND cs_i.bar_open_time >= f.bar_open_time - interval '5 minutes' "
           "AND cs_i.received_at <= f.received_at "
           "ORDER BY cs_i.bar_open_time DESC, cs_i.computed_at DESC, cs_i.bar_revision DESC LIMIT 1) cs ON true ";
}

Result<EconData> load_econ_data(persistence::PgPool& pool, const TrainerConfig& cfg,
                                const EconLabelInfo& labels, const std::string& exchange_code,
                                const econ::CostModelV2& cost_model) {
    EconData data;
    // Where the Bitget book spread sits in each frozen bitget_micro version:
    // the cost of a row is built from the reading AltTech published at its T0.
    auto spread_pos_r = spread_positions(pool);
    if (!spread_pos_r) return std::unexpected(spread_pos_r.error());
    const auto spread_pos = std::move(*spread_pos_r);
    // The target's own layout.
    SetInfo sets;
    {
        std::string error;
        auto res = pool.with_txn([&](pqxx::work& txn) {
            const auto fs = txn.exec_params(
                "SELECT id, schema_hash, frozen, engine_version FROM altml.fdw_feature_set_versions "
                "WHERE name = $1 AND version = $2",
                std::string(analytics::kCoreFeatureSetName), analytics::kCoreFeatureSetVersion);
            if (fs.empty() || !fs[0][2].as<bool>() ||
                fs[0][3].as<std::string>() != analytics::kFeatureEngineVersion) {
                error = "live feature set is not the frozen current engine";
                return;
            }
            sets.feature_set_id = fs[0][0].as<int64_t>();
            sets.feature_schema_hash = fs[0][1].as<std::string>();
            for (const auto& row : txn.exec_params(
                     "SELECT output_name FROM altml.fdw_feature_set_members "
                     "WHERE feature_set_version_id = $1 ORDER BY position",
                     sets.feature_set_id)) {
                sets.feature_names.push_back(row[0].as<std::string>());
            }
        });
        if (!res) return std::unexpected(res.error());
        if (!error.empty()) return fail(ErrorCode::validation_error, error);
    }
    data.feature_schema_hash = sets.feature_schema_hash;
    const auto& core_layout = sets.feature_names;
    const size_t core_width = core_layout.size();

    // Base: the scale-free projection of the target's own vector plus the
    // derived block computed from its full vector.
    std::vector<size_t> base_pos;
    for (const auto& name : recipe::projection_for(cfg.recipe_version)) {
        const auto it = std::find(core_layout.begin(), core_layout.end(), name);
        if (it == core_layout.end()) {
            return fail(ErrorCode::validation_error, "talib-core lacks projected column " + name);
        }
        base_pos.push_back(static_cast<size_t>(it - core_layout.begin()));
        data.base_projection.push_back(name);
    }
    recipe::append_base_names(data.base_projection, data.feature_names);
    const auto base_derived = recipe::DerivedRecipe::resolve(core_layout);
    if (!base_derived.ok) return fail(ErrorCode::validation_error, "derived inputs unresolved");
    data.base_width = static_cast<int32_t>(base_pos.size() + recipe::DerivedRecipe::width());

    // Sources.
    std::vector<EconSource> sources;
    for (const auto& spec : cfg.econ_sources) {
        const auto parsed = training::parse_aligned_source(spec, sources.size());
        if (!parsed) return fail(ErrorCode::validation_error, "unusable econ source: " + spec);
        EconSource es;
        es.src = *parsed;
        if (!recipe::source_method_compatible(cfg.recipe_version, es.src.feature_set, es.src.engine_version))
            return fail(ErrorCode::validation_error, "source method unpinned or incompatible: " + spec);
        std::vector<std::string> layout;
        if (es.src.feature_set == analytics::kCoreFeatureSetName) {
            if (es.src.feature_set_version != analytics::kCoreFeatureSetVersion) {
                return fail(ErrorCode::validation_error, "talib-core source version mismatch: " + spec);
            }
            layout = core_layout;
            es.src.equivalent_versions = {analytics::kCoreFeatureSetVersion};
            for (const auto& name : recipe::projection_for(cfg.recipe_version)) {
                const auto it = std::find(layout.begin(), layout.end(), name);
                es.src.projection.push_back(name);
                es.src.positions.push_back(static_cast<size_t>(it - layout.begin()));
            }
            es.derived = true;
            es.recipe = recipe::DerivedRecipe::resolve(layout);
            if (!es.recipe.ok) return fail(ErrorCode::validation_error, "derived unresolved: " + spec);
        } else {
            auto loaded = load_set_layout(pool, es.src.feature_set, es.src.feature_set_version);
            if (!loaded) return std::unexpected(loaded.error());
            layout = std::move(loaded->columns);
            if (!recipe::source_layout_compatible(es.src.feature_set, es.src.feature_set_version, layout))
                return fail(ErrorCode::validation_error, "unsupported econ source layout: " + spec);
            es.src.equivalent_versions = std::move(loaded->equivalent_versions);
            for (size_t i = 0; i < layout.size(); ++i) {
                if (recipe::is_excluded(es.src.feature_set, layout[i])) continue;
                es.src.projection.push_back(layout[i]);
                es.src.positions.push_back(i);
            }
        }
        if (es.src.projection.empty()) {
            return fail(ErrorCode::validation_error, "econ source contributes nothing: " + spec);
        }
        es.src.published_width = layout.size();
        recipe::append_source_names(
            recipe::source_prefix(es.src.exchange_id, es.src.timeframe, es.src.feature_set),
            es.src.projection, es.derived, data.feature_names);
        data.source_specs.push_back(training::aligned_source_spec(es.src));
        sources.push_back(std::move(es));
    }
    data.n_features = static_cast<int32_t>(data.feature_names.size());

    struct Instrument { int64_t exchange_id; int64_t instrument_id; std::string symbol; };
    std::vector<Instrument> instruments;
    if (auto r = pool.with_txn([&](pqxx::work& txn) {
            for (const auto& row : txn.exec_params(
                     "SELECT ei.exchange_id, ei.instrument_id, ei.exchange_symbol "
                     "FROM altml.fdw_exchange_instruments ei JOIN altml.fdw_exchanges e "
                     "ON e.id = ei.exchange_id "
                     "WHERE e.code = $1 AND ei.is_whitelisted AND ei.status = 'active' "
                     "ORDER BY ei.exchange_symbol",
                     exchange_code)) {
                instruments.push_back({row[0].as<int64_t>(), row[1].as<int64_t>(),
                                       row[2].as<std::string>()});
            }
        });
        !r) {
        return std::unexpected(r.error());
    }
    if (instruments.empty()) return fail(ErrorCode::validation_error, "no active instruments");

    const int64_t day_ms = 86'400'000;
    const int64_t date_to = (timeutil::now_ms() / day_ms) * day_ms;
    const int64_t date_from = date_to - cfg.lookback_days * day_ms;
    const std::string where =
        " WHERE c.is_closed AND c.quality_status = 'ok' "
        "AND l.computed_at >= c.updated_at AND f.quality_status = 'VALID' "
        "AND f.computed_at >= c.updated_at "
        "AND f.engine_version = '" + std::string(analytics::kFeatureEngineVersion) + "' "
        "AND f.exchange_id = $1 AND f.instrument_id = $2 AND f.timeframe = '1m' "
        "AND f.schema_hash = $3 AND l.label_set_version_id = $4 "
        "AND f.bar_open_time >= to_timestamp($5::bigint / 1000.0) "
        "AND f.bar_open_time < to_timestamp($6::bigint / 1000.0) "
        "AND mod(floor(extract(epoch FROM f.bar_open_time) / 60)::bigint, $7) = 0 ";
    const std::string from =
        " FROM altml.econ_feature_vectors f "
        "JOIN altml.labels l ON l.exchange_id = f.exchange_id "
        " AND l.instrument_id = f.instrument_id AND l.timeframe = f.timeframe "
        " AND l.bar_open_time = f.bar_open_time AND l.bar_revision = f.bar_revision " +
        quality_join_clause();

    // Stride against the row and memory ceilings. The LATERALs never change
    // the row count (they are LEFT joins), so the count runs without them.
    const int64_t row_cap =
        training::training_row_cap(cfg.max_rows, cfg.max_matrix_mib, data.n_features);
    int64_t stride = cfg.sample_every_minutes;
    int64_t candidate = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        candidate = 0;
        for (const auto& inst : instruments) {
            auto r = pool.with_txn([&](pqxx::work& txn) {
                txn.exec("SET LOCAL statement_timeout = '300s'");
                candidate += txn.exec_params("SELECT count(*)" + from + where, inst.exchange_id,
                                             inst.instrument_id, data.feature_schema_hash,
                                             labels.set_id, date_from, date_to, stride)[0][0]
                                 .as<int64_t>();
            });
            if (!r) return std::unexpected(r.error());
        }
        if (candidate <= row_cap || stride >= 24 * 60) break;
        stride = std::min<int64_t>(24 * 60, stride * std::max<int64_t>(2, (candidate + row_cap - 1) / row_cap));
    }
    data.sample_minutes = stride;
    log::info("econ_training_rows", {{"candidate_rows", candidate}, {"row_cap", row_cap},
                                     {"sample_every_minutes", stride},
                                     {"features", static_cast<int64_t>(data.n_features)}});
    if (candidate > row_cap) {
        return fail(ErrorCode::validation_error, "econ dataset exceeds the memory budget");
    }

    std::string cols = "SELECT (extract(epoch FROM f.bar_open_time) * 1000)::bigint, "
                       "f.values::text, f.validity_bitmap::text, l.values::text, "
                       "l.validity_bitmap::text, cs.spread::float8";
    std::string joins = spread_lateral_sql(spread_pos);
    for (const auto& es : sources) {
        cols += ", " + es.src.alias + ".values::text, " + es.src.alias + ".bmp::text";
        joins += training::aligned_source_sql_pit(es.src);
    }
    const std::string load_sql = cols + from + joins + where + "ORDER BY f.bar_open_time";

    data.X.reserve(static_cast<size_t>(candidate) * static_cast<size_t>(data.n_features));
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> derived(recipe::DerivedRecipe::width());
    for (const auto& inst : instruments) {
        if (ShutdownToken::instance().stop_requested()) {
            return fail(ErrorCode::shutdown_requested, "shutdown during data load", true);
        }
        const auto sym_id = static_cast<int32_t>(data.symbols.size());
        size_t kept = 0;
        auto r = pool.with_txn([&](pqxx::work& txn) {
            txn.exec("SET LOCAL statement_timeout = '600s'");
            for (const auto& row : txn.exec_params(load_sql, inst.exchange_id, inst.instrument_id,
                                                   data.feature_schema_hash, labels.set_id,
                                                   date_from, date_to, stride)) {
                const auto fvals = training::parse_pg_float_array(row[1].as<std::string>());
                const auto fbmp = training::parse_pg_hex_bytea(row[2].as<std::string>());
                const auto lvals = training::parse_pg_float_array(row[3].as<std::string>());
                const auto lbmp = training::parse_pg_hex_bytea(row[4].as<std::string>());
                if (fvals.size() != core_width || lvals.size() != labels.width) continue;
                // Outcomes first: a row with no complete horizon is not a sample.
                std::array<int, 3> yk{-1, -1, -1};
                std::array<double, 3> rk{}, mfek{}, maek{};
                bool any = false;
                for (size_t h = 0; h < 3; ++h) {
                    const auto& p = labels.pos[h];
                    if (!training::bitmap_bit(lbmp, p[0]) || !training::bitmap_bit(lbmp, p[1]) ||
                        !training::bitmap_bit(lbmp, p[2]) || !training::bitmap_bit(lbmp, p[3])) {
                        continue;
                    }
                    const float cls = lvals[p[0]];
                    if (!(cls == 0.0f || cls == 1.0f || cls == 2.0f)) continue;
                    yk[h] = static_cast<int>(cls);
                    rk[h] = lvals[p[1]];
                    mfek[h] = lvals[p[2]];
                    maek[h] = lvals[p[3]];
                    any = true;
                }
                if (!any) continue;
                // Cost v2 at this row's T0, and the economic class against it:
                // the stored class was drawn against the flat v1 figure.
                const double spread_obs = row[5].is_null() ? std::nan("") : row[5].as<double>();
                const auto cost_at = cost_model.at(spread_obs);
                for (size_t h = 0; h < 3; ++h) {
                    if (yk[h] >= 0) yk[h] = econ::econ_class(rk[h], cost_at.fraction());
                }
                const auto full = with_validity(fvals, fbmp);
                for (const size_t p : base_pos) data.X.push_back(full[p]);
                base_derived.compute(full.data(), full.size(), derived.data());
                data.X.insert(data.X.end(), derived.begin(), derived.end());
                for (size_t si = 0; si < sources.size(); ++si) {
                    const auto& es = sources[si];
                    const auto& vcell = row[static_cast<pqxx::row::size_type>(6 + 2 * si)];
                    const auto& bcell = row[static_cast<pqxx::row::size_type>(7 + 2 * si)];
                    const size_t width = es.src.positions.size() +
                                         (es.derived ? recipe::DerivedRecipe::width() : 0);
                    if (vcell.is_null() || bcell.is_null()) {
                        data.X.insert(data.X.end(), width, kNan);
                        continue;
                    }
                    const auto sfull =
                        with_validity(training::parse_pg_float_array(vcell.as<std::string>()),
                                      training::parse_pg_hex_bytea(bcell.as<std::string>()));
                    for (const size_t p : es.src.positions) {
                        data.X.push_back(p < sfull.size() ? sfull[p] : kNan);
                    }
                    if (es.derived) {
                        es.recipe.compute(sfull.data(), sfull.size(), derived.data());
                        data.X.insert(data.X.end(), derived.begin(), derived.end());
                    }
                }
                for (size_t h = 0; h < 3; ++h) {
                    data.y[h].push_back(yk[h]);
                    data.r[h].push_back(rk[h]);
                    data.mfe[h].push_back(mfek[h]);
                    data.mae[h].push_back(maek[h]);
                }
                data.cost.push_back(cost_at.fraction());
                data.spread_bps.push_back(static_cast<float>(cost_at.spread_bps));
                data.spread_observed.push_back(cost_at.spread_observed ? 1 : 0);
                data.t_ms.push_back(row[0].as<int64_t>());
                data.sym_ids.push_back(sym_id);
                ++kept;
            }
        });
        if (!r) return std::unexpected(r.error());
        if (kept > 0) data.symbols.push_back(inst.symbol);
    }
    if (data.X.size() != data.rows() * static_cast<size_t>(data.n_features)) {
        return fail(ErrorCode::internal_error, "econ matrix shape mismatch");
    }
    return data;
}

// ---- LightGBM, multiclass

std::string econ_lgbm_params(const TrainerConfig& cfg) {
    std::ostringstream out;
    out << "objective=multiclass num_class=3 metric=multi_logloss learning_rate="
        << cfg.learning_rate << " num_leaves=" << cfg.num_leaves
        << " min_data_in_leaf=" << cfg.min_data_in_leaf
        << " feature_fraction=" << cfg.feature_fraction
        << " bagging_fraction=" << cfg.bagging_fraction << " bagging_freq=" << cfg.bagging_freq
        << " lambda_l1=" << cfg.lambda_l1 << " lambda_l2=" << cfg.lambda_l2
        << " max_depth=" << cfg.max_depth << " num_threads=" << cfg.threads
        << " verbosity=-1 seed=42 deterministic=true max_bin=255";
    return out.str();
}

std::vector<float> gather_econ(const EconData& d, const std::vector<int64_t>& idx) {
    const auto n = static_cast<size_t>(d.n_features);
    std::vector<float> out(idx.size() * n);
    for (size_t r = 0; r < idx.size(); ++r) {
        std::copy_n(d.X.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(idx[r]) * n), n,
                    out.begin() + static_cast<std::ptrdiff_t>(r * n));
    }
    return out;
}

Result<void> econ_dataset(const EconData& d, const std::vector<int64_t>& idx, size_t h,
                          const std::string& params, DatasetHandle ref, LgbmDataset& out) {
    const auto rows = gather_econ(d, idx);
    if (LGBM_DatasetCreateFromMat(rows.data(), C_API_DTYPE_FLOAT32, static_cast<int32_t>(idx.size()),
                                  d.n_features, 1, params.c_str(), ref, &out.handle) != 0) {
        return fail(ErrorCode::internal_error, std::string("LGBM dataset: ") + LGBM_GetLastError());
    }
    std::vector<float> lab(idx.size());
    for (size_t i = 0; i < idx.size(); ++i) {
        lab[i] = static_cast<float>(d.y[h][static_cast<size_t>(idx[i])]);
    }
    if (LGBM_DatasetSetField(out.handle, "label", lab.data(), static_cast<int>(lab.size()),
                             C_API_DTYPE_FLOAT32) != 0) {
        return fail(ErrorCode::internal_error, std::string("LGBM label: ") + LGBM_GetLastError());
    }
    return {};
}

// Early stopping on the validation multi-logloss (lower is better).
Result<void> econ_train(const EconData& d, size_t h, const std::vector<int64_t>& tr,
                        const std::vector<int64_t>& va, const TrainerConfig& cfg,
                        std::chrono::steady_clock::time_point deadline, int64_t fixed_iterations,
                        LgbmBooster& out, const std::string& ignore = {}) {
    std::string params = econ_lgbm_params(cfg);
    if (!ignore.empty()) params += " ignore_column=" + ignore;
    LgbmDataset train_set, valid_set;
    if (auto r = econ_dataset(d, tr, h, params, nullptr, train_set); !r) return r;
    const bool with_valid = !va.empty() && fixed_iterations <= 0;
    if (with_valid) {
        if (auto r = econ_dataset(d, va, h, params, train_set.handle, valid_set); !r) return r;
    }
    if (LGBM_BoosterCreate(train_set.handle, params.c_str(), &out.handle) != 0) {
        return fail(ErrorCode::internal_error, std::string("LGBM booster: ") + LGBM_GetLastError());
    }
    if (with_valid && LGBM_BoosterAddValidData(out.handle, valid_set.handle) != 0) {
        return fail(ErrorCode::internal_error, std::string("LGBM valid: ") + LGBM_GetLastError());
    }
    const int64_t max_iter = fixed_iterations > 0 ? fixed_iterations : cfg.max_iterations;
    double best = std::numeric_limits<double>::infinity();
    int best_iter = 0, since = 0;
    for (int it = 1; it <= max_iter; ++it) {
        if (ShutdownToken::instance().stop_requested()) {
            return fail(ErrorCode::shutdown_requested, "shutdown during boosting", true);
        }
        int finished = 0;
        if (LGBM_BoosterUpdateOneIter(out.handle, &finished) != 0) {
            return fail(ErrorCode::internal_error, std::string("LGBM iter: ") + LGBM_GetLastError());
        }
        if (with_valid) {
            int len = 0;
            double loss = 0;
            if (LGBM_BoosterGetEval(out.handle, 1, &len, &loss) != 0 || len < 1) {
                return fail(ErrorCode::internal_error, std::string("LGBM eval: ") + LGBM_GetLastError());
            }
            if (loss < best) { best = loss; best_iter = it; since = 0; }
            else if (++since >= cfg.early_stopping_rounds) break;
        } else {
            best_iter = it;
        }
        if (finished != 0) break;
        if (std::chrono::steady_clock::now() > deadline) {
            if (best_iter >= cfg.min_iterations) break;
            return fail(ErrorCode::timeout, "time budget exhausted before minimum iterations");
        }
    }
    if (best_iter < 1) return fail(ErrorCode::internal_error, "no boosting iterations completed");
    out.best_iteration = best_iter;
    return {};
}

// ---- magnitude regressions (up / down excursion) -------------------------
//
// VPS1 publishes how FAR the market is expected to move, not only which way:
// the expected maximum excursion up and down over the horizon, each from its
// own LightGBM regression on log(1 + excursion in bps). The multiclass model's
// class means give a magnitude too (sum_k p_k * mean excursion of class k), but
// it can only vary through the three probabilities; a dedicated regression
// reads the volatility state directly. Which source a model publishes is chosen
// on its VALIDATION slice (never the holdout) and stated on every prediction.

std::string econ_reg_params(const TrainerConfig& cfg) {
    std::ostringstream out;
    out << "objective=regression metric=l2 learning_rate=" << cfg.learning_rate
        << " num_leaves=" << cfg.num_leaves << " min_data_in_leaf=" << cfg.min_data_in_leaf
        << " feature_fraction=" << cfg.feature_fraction
        << " bagging_fraction=" << cfg.bagging_fraction << " bagging_freq=" << cfg.bagging_freq
        << " lambda_l1=" << cfg.lambda_l1 << " lambda_l2=" << cfg.lambda_l2
        << " max_depth=" << cfg.max_depth << " num_threads=" << cfg.threads
        << " verbosity=-1 seed=42 deterministic=true max_bin=255";
    return out.str();
}

// The two magnitude targets of row i, log(1 + bps); NaN where the path is unknown.
double up_target(const EconData& d, size_t h, size_t i) {
    const double m = d.mfe[h][i];
    return std::isfinite(m) ? std::log1p(std::max(std::expm1(m), 0.0) * 1e4) : std::nan("");
}
double down_target(const EconData& d, size_t h, size_t i) {
    const double m = d.mae[h][i];
    return std::isfinite(m) ? std::log1p(std::max(-std::expm1(m), 0.0) * 1e4) : std::nan("");
}

std::vector<int64_t> finite_rows(const std::vector<int64_t>& idx, const std::vector<double>& target) {
    std::vector<int64_t> out;
    out.reserve(idx.size());
    for (const int64_t i : idx) {
        if (std::isfinite(target[static_cast<size_t>(i)])) out.push_back(i);
    }
    return out;
}

Result<void> reg_dataset(const EconData& d, const std::vector<int64_t>& idx, const std::vector<double>& target,
                         const std::string& params, DatasetHandle ref, LgbmDataset& out) {
    const auto rows = gather_econ(d, idx);
    if (LGBM_DatasetCreateFromMat(rows.data(), C_API_DTYPE_FLOAT32, static_cast<int32_t>(idx.size()),
                                  d.n_features, 1, params.c_str(), ref, &out.handle) != 0) {
        return fail(ErrorCode::internal_error, std::string("LGBM dataset: ") + LGBM_GetLastError());
    }
    std::vector<float> lab(idx.size());
    for (size_t i = 0; i < idx.size(); ++i) lab[i] = static_cast<float>(target[static_cast<size_t>(idx[i])]);
    if (LGBM_DatasetSetField(out.handle, "label", lab.data(), static_cast<int>(lab.size()),
                             C_API_DTYPE_FLOAT32) != 0) {
        return fail(ErrorCode::internal_error, std::string("LGBM label: ") + LGBM_GetLastError());
    }
    return {};
}

// Early stopping on the validation L2 (lower is better), or a fixed number of
// iterations for the refit.
Result<void> reg_train(const EconData& d, const std::vector<double>& target, const std::vector<int64_t>& tr_all,
                       const std::vector<int64_t>& va_all, const TrainerConfig& cfg,
                       std::chrono::steady_clock::time_point deadline, int64_t fixed_iterations,
                       LgbmBooster& out) {
    const auto tr = finite_rows(tr_all, target);
    const auto va = finite_rows(va_all, target);
    if (tr.size() < 1000) return fail(ErrorCode::validation_error, "too few rows for a magnitude model");
    const std::string params = econ_reg_params(cfg);
    LgbmDataset train_set, valid_set;
    if (auto r = reg_dataset(d, tr, target, params, nullptr, train_set); !r) return r;
    const bool with_valid = !va.empty() && fixed_iterations <= 0;
    if (with_valid) {
        if (auto r = reg_dataset(d, va, target, params, train_set.handle, valid_set); !r) return r;
    }
    if (LGBM_BoosterCreate(train_set.handle, params.c_str(), &out.handle) != 0) {
        return fail(ErrorCode::internal_error, std::string("LGBM booster: ") + LGBM_GetLastError());
    }
    if (with_valid && LGBM_BoosterAddValidData(out.handle, valid_set.handle) != 0) {
        return fail(ErrorCode::internal_error, std::string("LGBM valid: ") + LGBM_GetLastError());
    }
    const int64_t max_iter = fixed_iterations > 0 ? fixed_iterations : cfg.max_iterations;
    double best = std::numeric_limits<double>::infinity();
    int best_iter = 0, since = 0;
    for (int it = 1; it <= max_iter; ++it) {
        if (ShutdownToken::instance().stop_requested()) {
            return fail(ErrorCode::shutdown_requested, "shutdown during boosting", true);
        }
        int finished = 0;
        if (LGBM_BoosterUpdateOneIter(out.handle, &finished) != 0) {
            return fail(ErrorCode::internal_error, std::string("LGBM iter: ") + LGBM_GetLastError());
        }
        if (with_valid) {
            int len = 0;
            double loss = 0;
            if (LGBM_BoosterGetEval(out.handle, 1, &len, &loss) != 0 || len < 1) {
                return fail(ErrorCode::internal_error, std::string("LGBM eval: ") + LGBM_GetLastError());
            }
            if (loss < best) { best = loss; best_iter = it; since = 0; }
            else if (++since >= cfg.early_stopping_rounds) break;
        } else {
            best_iter = it;
        }
        if (finished != 0) break;
        if (std::chrono::steady_clock::now() > deadline) {
            if (best_iter >= cfg.min_iterations) break;
            return fail(ErrorCode::timeout, "time budget exhausted before minimum iterations");
        }
    }
    if (best_iter < 1) return fail(ErrorCode::internal_error, "no boosting iterations completed");
    out.best_iteration = best_iter;
    return {};
}

Result<std::vector<double>> reg_predict(BoosterHandle b, const EconData& d, const std::vector<int64_t>& idx,
                                        int iters) {
    const auto rows = gather_econ(d, idx);
    std::vector<double> out(idx.size());
    int64_t len = 0;
    if (LGBM_BoosterPredictForMat(b, rows.data(), C_API_DTYPE_FLOAT32, static_cast<int32_t>(idx.size()),
                                  d.n_features, 1, C_API_PREDICT_NORMAL, 0, iters, "", &len, out.data()) != 0 ||
        len != static_cast<int64_t>(out.size())) {
        return fail(ErrorCode::internal_error, std::string("LGBM predict: ") + LGBM_GetLastError());
    }
    return out;
}

// Raw multiclass margins, n x 3 row-major.
Result<std::vector<double>> econ_raw(BoosterHandle b, const EconData& d,
                                     const std::vector<int64_t>& idx, int iters) {
    const auto rows = gather_econ(d, idx);
    std::vector<double> out(idx.size() * 3);
    int64_t len = 0;
    if (LGBM_BoosterPredictForMat(b, rows.data(), C_API_DTYPE_FLOAT32,
                                  static_cast<int32_t>(idx.size()), d.n_features, 1,
                                  C_API_PREDICT_RAW_SCORE, 0, iters, "", &len, out.data()) != 0 ||
        len != static_cast<int64_t>(out.size())) {
        return fail(ErrorCode::internal_error, std::string("LGBM predict: ") + LGBM_GetLastError());
    }
    return out;
}

struct EconScored {
    econ::SliceScore score;
    std::vector<double> p;      // calibrated, n x 3, aligned with the slice
    std::vector<int> side;      // per slice row
    std::vector<econ::Estimate> est;
};

// The calibrated estimate of every row of a slice, each netted with the cost
// estimated at its own T0.
std::vector<econ::Estimate> slice_estimates(const EconData& d, const std::vector<int64_t>& idx,
                                            const std::vector<double>& p,
                                            const econ::ClassMeans& means) {
    std::vector<econ::Estimate> est(idx.size());
    for (size_t i = 0; i < idx.size(); ++i) {
        est[i] = econ::estimate({p[i * 3], p[i * 3 + 1], p[i * 3 + 2]}, means,
                                d.cost[static_cast<size_t>(idx[i])]);
    }
    return est;
}

// Score one slice with calibrated probabilities `p` (n x 3, aligned to idx).
// Decisions use the policy margin chosen on the slice's validation data; with
// no margin the policy takes no position at all.
EconScored score_slice(const EconData& d, size_t h, const std::vector<int64_t>& idx,
                       std::vector<double> p, const std::array<double, 3>& climate,
                       const econ::ClassMeans& means, std::optional<double> margin_bps,
                       int64_t hold_ms) {
    EconScored s;
    std::vector<int> ys(idx.size());
    for (size_t i = 0; i < idx.size(); ++i) ys[i] = d.y[h][static_cast<size_t>(idx[i])];
    const auto clim = econ::climatology(climate, idx.size());
    s.score.logloss_skill = 1.0 - econ::multi_logloss(p, ys) / econ::multi_logloss(clim, ys);
    s.score.brier_skill = 1.0 - econ::multi_brier(p, ys) / econ::multi_brier(clim, ys);
    s.score.ece_long = econ::ece_class(p, ys, econ::kLong);
    s.score.ece_short = econ::ece_class(p, ys, econ::kShort);
    {
        std::vector<double> lr(idx.size());
        for (size_t i = 0; i < idx.size(); ++i) lr[i] = d.r[h][static_cast<size_t>(idx[i])];
        s.score.direction_auc = econ::direction_auc(p, lr);
    }
    // Decisions on the calibrated estimate; economics on the realised path.
    std::vector<int> side_full(d.rows(), 0);
    s.side.resize(idx.size());
    s.est.resize(idx.size());
    s.est = slice_estimates(d, idx, p, means);
    for (size_t i = 0; i < idx.size(); ++i) {
        s.side[i] = margin_bps ? econ::decide_side(s.est[i], *margin_bps / 10'000.0) : 0;
        side_full[static_cast<size_t>(idx[i])] = s.side[i];
    }
    const auto selected = econ::select_per_instrument(idx, d.t_ms, d.sym_ids, side_full, hold_ms);
    s.score.econ = econ::evaluate(selected, side_full, d.r[h], d.mfe[h], d.mae[h], d.t_ms, d.cost);
    s.p = std::move(p);
    return s;
}

json slice_json(const econ::SliceScore& s) {
    const auto num = [](double v) { return std::isfinite(v) ? json(v) : json(nullptr); };
    return json{{"logloss_skill", num(s.logloss_skill)},
                {"brier_skill", num(s.brier_skill)},
                {"direction_auc", num(s.direction_auc)},
                {"ece_long", num(s.ece_long)},
                {"ece_short", num(s.ece_short)},
                {"trades", s.econ.trades},
                {"mean_net_bps", num(s.econ.mean_net * 1e4)},
                {"mean_gross_bps", num(s.econ.mean_gross * 1e4)},
                {"t_day", num(s.econ.t_day)},
                {"hit_rate", num(s.econ.hit_rate)},
                {"long_share", num(s.econ.long_share)},
                {"profit_factor", num(s.econ.profit_factor)},
                {"max_drawdown_units", num(s.econ.max_drawdown)},
                {"mean_mfe_bps", num(s.econ.mean_mfe * 1e4)},
                {"mean_mae_bps", num(s.econ.mean_mae * 1e4)},
                {"days", s.econ.days}};
}

// Rows with an outcome for horizon h, as indices into the full matrix.
std::vector<int64_t> rows_with_outcome(const EconData& d, size_t h) {
    std::vector<int64_t> out;
    for (size_t i = 0; i < d.rows(); ++i) {
        if (d.y[h][i] >= 0) out.push_back(static_cast<int64_t>(i));
    }
    return out;
}

std::vector<int64_t> remap(const std::vector<int64_t>& local, const std::vector<int64_t>& global) {
    std::vector<int64_t> out(local.size());
    for (size_t i = 0; i < local.size(); ++i) out[i] = global[static_cast<size_t>(local[i])];
    return out;
}

Result<CycleOutcome> run_econ_horizon(persistence::PgPool& pool, const config::RuntimeConfig& rt,
                                      const TrainerConfig& cfg, const EconData& d, size_t h,
                                      const econ::CostModelV2& cost_model, int fee_model_version) {
    const auto& hz = econ::kHorizons[h];
    const std::string objective = econ::objective_of(hz.name);
    const int64_t hold_ms = static_cast<int64_t>(hz.minutes) * 60'000;
    // Purge/embargo covers the horizon: a label reads hz.minutes of future.
    const int64_t embargo_ms = econ::embargo_ms(cfg.embargo_minutes, hz.minutes);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(cfg.econ_time_budget_sec);

    const auto rows = rows_with_outcome(d, h);
    if (static_cast<int64_t>(rows.size()) < cfg.min_rows) {
        return fail(ErrorCode::validation_error,
                    std::string(hz.name) + ": insufficient labeled history rows=" +
                        std::to_string(rows.size()),
                    true);
    }
    std::vector<int64_t> times(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) times[i] = d.t_ms[static_cast<size_t>(rows[i])];

    std::vector<training::FoldIdx> folds;
    training::FinalSplitIdx fin;
    try {
        folds = training::build_walk_forward_folds(times, static_cast<size_t>(cfg.folds), embargo_ms);
        fin = training::build_final_split(times, static_cast<size_t>(cfg.folds), embargo_ms,
                                          cfg.validation_fraction);
    } catch (const std::exception& e) {
        return fail(ErrorCode::validation_error, std::string("temporal split: ") + e.what());
    }

    const auto fit_calibrated = [&](const std::vector<int64_t>& tr, const std::vector<int64_t>& va,
                                    const std::string& ignore, LgbmBooster& booster,
                                    double& temperature) -> Result<void> {
        if (auto r = econ_train(d, h, tr, va, cfg, deadline, 0, booster, ignore); !r) return r;
        auto raw = econ_raw(booster.handle, d, va, booster.best_iteration);
        if (!raw) return std::unexpected(raw.error());
        std::vector<int> yv(va.size());
        for (size_t i = 0; i < va.size(); ++i) yv[i] = d.y[h][static_cast<size_t>(va[i])];
        temperature = econ::fit_temperature(*raw, yv);
        return {};
    };
    const auto calibrated = [&](BoosterHandle b, const std::vector<int64_t>& idx, int iters,
                                double t) -> Result<std::vector<double>> {
        auto raw = econ_raw(b, d, idx, iters);
        if (!raw) return std::unexpected(raw.error());
        std::vector<double> p(raw->size());
        for (size_t i = 0; i < idx.size(); ++i) {
            const auto q = econ::softmax3(&(*raw)[i * 3], t);
            p[i * 3] = q[0]; p[i * 3 + 1] = q[1]; p[i * 3 + 2] = q[2];
        }
        return p;
    };

    // ---- walk-forward
    std::vector<econ::SliceScore> fold_scores;
    json folds_json = json::array();
    for (size_t k = 0; k < folds.size(); ++k) {
        const auto tr = remap(folds[k].train, rows), va = remap(folds[k].valid, rows),
                   te = remap(folds[k].test, rows);
        const auto sel = training::select_features_fold_local(d.X, static_cast<size_t>(d.n_features), tr);
        std::string ignore;
        for (size_t f = 0; f < sel.reason.size(); ++f) {
            if (sel.reason[f] == training::RejectReason::kept) continue;
            if (!ignore.empty()) ignore += ',';
            ignore += std::to_string(f);
        }
        LgbmBooster b;
        double t = 1.0;
        if (auto r = fit_calibrated(tr, va, ignore, b, t); !r) return std::unexpected(r.error());
        auto p = calibrated(b.handle, te, b.best_iteration, t);
        if (!p) return std::unexpected(p.error());
        const auto climate = econ::class_frequencies(d.y[h], tr);
        const auto means = econ::class_means(d.y[h], d.r[h], d.mfe[h], d.mae[h], tr);
        if (!means.complete()) {
            return fail(ErrorCode::validation_error, "fold " + std::to_string(k) + " lacks a class");
        }
        // The policy margin, chosen on this fold's validation slice (the one
        // its temperature was fitted on), then applied to its test slice.
        auto pva = calibrated(b.handle, va, b.best_iteration, t);
        if (!pva) return std::unexpected(pva.error());
        const auto fold_margin = econ::select_margin(va, slice_estimates(d, va, *pva, means), d.t_ms,
                                                     d.sym_ids, d.r[h], d.mfe[h], d.mae[h], d.cost,
                                                     hold_ms);
        auto s = score_slice(d, h, te, std::move(*p), climate, means, fold_margin.margin_bps, hold_ms);
        log::info("econ_fold_done", {{"horizon", std::string(hz.name)},
                                     {"fold", static_cast<int64_t>(k)},
                                     {"logloss_skill", s.score.logloss_skill},
                                     {"trades", s.score.econ.trades},
                                     {"mean_net_bps", s.score.econ.mean_net * 1e4}});
        folds_json.push_back({{"fold", k},
                              {"temperature", t},
                              {"margin_bps", fold_margin.margin_bps ? json(*fold_margin.margin_bps) : json(nullptr)},
                              {"iterations", b.best_iteration},
                              {"selected_features", static_cast<int64_t>(sel.selected.size())},
                              {"train_from", d.t_ms[static_cast<size_t>(tr.front())]},
                              {"test_from", d.t_ms[static_cast<size_t>(te.front())]},
                              {"test_to", d.t_ms[static_cast<size_t>(te.back())]},
                              {"metrics", slice_json(s.score)}});
        fold_scores.push_back(s.score);
    }

    // ---- final: tune on train/valid, calibrate on valid, refit on everything
    // before the holdout, score the holdout once.
    const auto ftr = remap(fin.train, rows), fva = remap(fin.valid, rows),
               ffit = remap(fin.fit, rows), fte = remap(fin.test, rows);
    LgbmBooster tuned;
    double temperature = 1.0;
    if (auto r = fit_calibrated(ftr, fva, {}, tuned, temperature); !r) return std::unexpected(r.error());
    LgbmBooster final_model;
    if (auto r = econ_train(d, h, ffit, {}, cfg, deadline, tuned.best_iteration, final_model); !r) {
        return std::unexpected(r.error());
    }
    const auto climate = econ::class_frequencies(d.y[h], ffit);
    const auto means = econ::class_means(d.y[h], d.r[h], d.mfe[h], d.mae[h], ffit);
    if (!means.complete()) return fail(ErrorCode::validation_error, "fit rows lack a class");
    // The policy margin: chosen with the TUNED model (trained on ftr only) on
    // the validation slice fva, with ftr's class means -- the refit model has
    // seen fva, so choosing with it would be in-sample. Then fixed.
    econ::MarginChoice policy;
    {
        const auto tmeans = econ::class_means(d.y[h], d.r[h], d.mfe[h], d.mae[h], ftr);
        auto pva = calibrated(tuned.handle, fva, tuned.best_iteration, temperature);
        if (!pva) return std::unexpected(pva.error());
        if (tmeans.complete()) {
            policy = econ::select_margin(fva, slice_estimates(d, fva, *pva, tmeans), d.t_ms, d.sym_ids,
                                         d.r[h], d.mfe[h], d.mae[h], d.cost, hold_ms);
        }
    }
    auto pte = calibrated(final_model.handle, fte, final_model.best_iteration, temperature);
    if (!pte) return std::unexpected(pte.error());
    auto holdout = score_slice(d, h, fte, std::move(*pte), climate, means, policy.margin_bps, hold_ms);

    // ---- magnitude: up / down excursion regressions (see econ_reg_params).
    // Tuned on ftr with early stopping on fva, refit on ffit with the tuned
    // iteration count -- exactly the classifier's protocol. The source a model
    // publishes (regression or the class means) is chosen on fva with the
    // TUNED models; the holdout only reports.
    std::vector<double> U(d.rows()), D(d.rows());
    for (size_t i = 0; i < d.rows(); ++i) { U[i] = up_target(d, h, i); D[i] = down_target(d, h, i); }
    LgbmBooster up_final, down_final;
    std::string magnitude_source = "class_means";
    json magnitude = {{"target", "log(1 + excursion in bps), excursion = max move up / down over the horizon"},
                      {"estimator", "expm1(prediction): a median-type magnitude"},
                      {"chosen_on", "validation slice (tuned models), Spearman against the realised excursion"}};
    {
        LgbmBooster up_tuned, down_tuned;
        auto ru = reg_train(d, U, ftr, fva, cfg, deadline, 0, up_tuned);
        auto rd = ru ? reg_train(d, D, ftr, fva, cfg, deadline, 0, down_tuned) : ru;
        if (ru && rd) {
            // Validation: the tuned regressions against the tuned classifier's
            // class-mean magnitudes (ftr means) on the same rows.
            const auto tmeans = econ::class_means(d.y[h], d.r[h], d.mfe[h], d.mae[h], ftr);
            auto pva = calibrated(tuned.handle, fva, tuned.best_iteration, temperature);
            auto pu = reg_predict(up_tuned.handle, d, fva, up_tuned.best_iteration);
            auto pd = reg_predict(down_tuned.handle, d, fva, down_tuned.best_iteration);
            if (!pva || !pu || !pd) return fail(ErrorCode::internal_error, "magnitude validation prediction failed");
            std::vector<double> cu(fva.size()), cd(fva.size()), tu(fva.size()), td(fva.size());
            for (size_t i = 0; i < fva.size(); ++i) {
                const auto e = econ::estimate({(*pva)[i * 3], (*pva)[i * 3 + 1], (*pva)[i * 3 + 2]}, tmeans, 0.0);
                cu[i] = std::log1p(std::max(e.expected_mfe_long, 0.0) * 1e4);
                cd[i] = std::log1p(std::max(-e.expected_mae_long, 0.0) * 1e4);
                tu[i] = U[static_cast<size_t>(fva[i])];
                td[i] = D[static_cast<size_t>(fva[i])];
            }
            const double v_ru = econ::spearman(*pu, tu), v_rd = econ::spearman(*pd, td);
            const double v_cu = econ::spearman(cu, tu), v_cd = econ::spearman(cd, td);
            magnitude["validation"] = {{"regression_up_spearman", v_ru}, {"regression_down_spearman", v_rd},
                                       {"class_means_up_spearman", v_cu}, {"class_means_down_spearman", v_cd}};
            if (v_ru >= v_cu && v_rd >= v_cd) {
                auto fu = reg_train(d, U, ffit, {}, cfg, deadline, up_tuned.best_iteration, up_final);
                auto fd = fu ? reg_train(d, D, ffit, {}, cfg, deadline, down_tuned.best_iteration, down_final) : fu;
                if (fu && fd) magnitude_source = "regression";
                else magnitude["refit_failed"] = (fu ? fd : fu).error().message;
            }
            magnitude["iterations"] = {{"up", up_tuned.best_iteration}, {"down", down_tuned.best_iteration}};
        } else {
            magnitude["unavailable"] = (ru ? rd : ru).error().message;
        }
    }
    // Holdout, reported for both sources.
    {
        std::vector<double> cu(fte.size()), cd(fte.size()), tu(fte.size()), td(fte.size());
        for (size_t i = 0; i < fte.size(); ++i) {
            cu[i] = std::log1p(std::max(holdout.est[i].expected_mfe_long, 0.0) * 1e4);
            cd[i] = std::log1p(std::max(-holdout.est[i].expected_mae_long, 0.0) * 1e4);
            tu[i] = U[static_cast<size_t>(fte[i])];
            td[i] = D[static_cast<size_t>(fte[i])];
        }
        json hj = {{"class_means_up_spearman", econ::spearman(cu, tu)},
                   {"class_means_down_spearman", econ::spearman(cd, td)}};
        if (magnitude_source == "regression") {
            auto pu = reg_predict(up_final.handle, d, fte, up_final.best_iteration);
            auto pd = reg_predict(down_final.handle, d, fte, down_final.best_iteration);
            if (!pu || !pd) return fail(ErrorCode::internal_error, "magnitude holdout prediction failed");
            hj["regression_up_spearman"] = econ::spearman(*pu, tu);
            hj["regression_down_spearman"] = econ::spearman(*pd, td);
        }
        magnitude["holdout"] = hj;
    }
    magnitude["source"] = magnitude_source;

    // Promotion is on MARKET prediction quality (proper scores against the
    // base rates, across folds and on the holdout). The policy-2 economic
    // gates are still evaluated -- and reported, as a diagnostic -- but gate
    // nothing: VPS1 predicts the market, VPS2 decides what pays.
    econ::MarketGateConfig mgates;
    mgates.min_positive_folds = cfg.min_positive_folds;
    auto gate_failures = econ::market_gate_failures(fold_scores, holdout.score, mgates);
    econ::GateConfig egates;
    egates.min_trades = cfg.min_trades;
    egates.min_holdout_t = cfg.econ_min_holdout_t;
    egates.min_positive_folds = cfg.min_positive_folds;
    const auto econ_diagnostic = econ::gate_failures(fold_scores, holdout.score, egates);
    // Is a probability a probability? (econ_logic.hpp::calibration_test)
    std::vector<int> y_holdout(fte.size());
    for (size_t i = 0; i < fte.size(); ++i) y_holdout[i] = d.y[h][static_cast<size_t>(fte[i])];
    const auto ctest = econ::calibration_test(holdout.p, y_holdout);

    // Champion-challenger: the current ACTIVE model of this objective, if it
    // reads the same feature space, is scored on the same holdout.
    std::string champion_id;
    double champion_net = std::nan("");
    double champion_ll = std::nan("");
    {
        std::string path, checksum, names_sha;
        json calib;
        auto res = pool.with_txn([&](pqxx::work& txn) {
            const auto r = txn.exec_params(
                "SELECT m.model_id, a.path, a.checksum, c.feature_names_sha256, c.calibration::text "
                "FROM altml.models m "
                "JOIN altml.model_artifacts a ON a.model_id = m.model_id AND a.kind = 'model' "
                "JOIN altml.model_feature_compatibility c ON c.model_id = m.model_id "
                "WHERE m.status = 'ACTIVE' AND m.algorithm = 'lightgbm' AND m.objective = $1 LIMIT 1",
                objective);
            if (!r.empty()) {
                champion_id = r[0][0].as<std::string>();
                path = r[0][1].as<std::string>();
                checksum = r[0][2].as<std::string>();
                names_sha = r[0][3].as<std::string>();
                calib = json::parse(r[0][4].as<std::string>(), nullptr, false);
            }
        });
        if (!res) return std::unexpected(res.error());
        if (!champion_id.empty() && names_sha == sha256_hex(recipe::joined_names(d.feature_names)) &&
            file_sha256(path) == checksum &&
            calib.is_object() && calib.contains("temperature") && calib.contains("class_means_return")) {
            BoosterHandle champ = nullptr;
            int iters = 0;
            if (LGBM_BoosterCreateFromModelfile(path.c_str(), &iters, &champ) == 0) {
                const double ct = calib["temperature"].get<double>();
                econ::ClassMeans cm;
                for (size_t k = 0; k < 3; ++k) {
                    cm.ret[k] = calib["class_means_return"][k].get<double>();
                    cm.n[k] = 1;
                }
                std::optional<double> cmargin = 0.0;   // policy-1 champions acted at > 0
                if (calib.contains("policy") && calib["policy"].is_object()) {
                    const auto& pm = calib["policy"]["margin_bps"];
                    cmargin = pm.is_number() ? std::optional<double>(pm.get<double>()) : std::nullopt;
                }
                if (auto cp = calibrated(champ, fte, -1, ct)) {
                    const auto cs = score_slice(d, h, fte, std::move(*cp), climate, cm, cmargin, hold_ms);
                    champion_net = cs.score.econ.mean_net;
                    champion_ll = cs.score.logloss_skill;
                }
                LGBM_BoosterFree(champ);
            }
        } else if (!champion_id.empty()) {
            log::warn("champion_not_comparable", {{"model_id", champion_id}});
            champion_id.clear();
        }
    }
    // Champion-challenger on the same holdout rows and the same proper score.
    if (!champion_id.empty() && std::isfinite(champion_ll) &&
        !(holdout.score.logloss_skill > champion_ll)) {
        gate_failures.emplace_back("challenger does not beat champion's holdout log-loss skill");
    }
    const bool gates_passed = gate_failures.empty();

    // What the model was fitted on, fingerprinted: the rows (time, instrument)
    // and their labels in fit order. Two models with the same fingerprint saw
    // the same data; the registry's dataset_id carries it.
    std::string dataset_fingerprint;
    {
        std::string buf;
        buf.reserve(ffit.size() * 13);
        for (const int64_t i64 : ffit) {
            const auto u = static_cast<size_t>(i64);
            buf.append(reinterpret_cast<const char*>(&d.t_ms[u]), sizeof(int64_t));
            const int32_t sym = d.sym_ids[u];
            buf.append(reinterpret_cast<const char*>(&sym), sizeof(int32_t));
            buf.push_back(static_cast<char>(d.y[h][u]));
        }
        dataset_fingerprint = sha256_hex(buf).substr(0, 16);
    }

    // ---- artifacts
    const std::string stamp = [] {
        std::string iso = timeutil::to_iso8601(timeutil::now_ms());
        std::string out;
        for (const char ch : iso) {
            if (ch == '-' || ch == ':') continue;
            if (ch == '.') break;
            out += ch;
        }
        return out + "Z";
    }();
    const std::string model_id = stamp + "-lightgbm-econ" + std::string(hz.name) + "-" + random_hex8();
    const fs::path root(rt.project.model_root);
    const fs::path tmp_dir = root / (".tmp-" + model_id);
    const fs::path final_dir = root / model_id;
    std::error_code fe;
    fs::create_directories(tmp_dir, fe);
    if (fe) return fail(ErrorCode::internal_error, "cannot create model directory: " + fe.message());
    const std::string model_path = (tmp_dir / "model.txt").string();
    if (LGBM_BoosterSaveModel(final_model.handle, 0, final_model.best_iteration,
                              C_API_FEATURE_IMPORTANCE_SPLIT, model_path.c_str()) != 0) {
        return fail(ErrorCode::internal_error, std::string("LGBM save: ") + LGBM_GetLastError());
    }
    // Write/read parity: the artifact that will serve live reproduces the
    // in-memory calibrated probabilities on the holdout's first rows.
    {
        std::vector<int64_t> sample(fte.begin(), fte.begin() + std::min<std::ptrdiff_t>(
                                                                 kSampleRows, static_cast<std::ptrdiff_t>(fte.size())));
        BoosterHandle re = nullptr;
        int re_iters = 0;
        if (LGBM_BoosterCreateFromModelfile(model_path.c_str(), &re_iters, &re) != 0) {
            return fail(ErrorCode::internal_error, std::string("saved model does not load: ") +
                                                       LGBM_GetLastError());
        }
        auto rp = calibrated(re, sample, -1, temperature);
        LGBM_BoosterFree(re);
        if (!rp) return std::unexpected(rp.error());
        for (size_t i = 0; i < rp->size(); ++i) {
            if (std::abs((*rp)[i] - holdout.p[i]) > kSelfCheckTolerance) {
                return fail(ErrorCode::validation_error, "write/read parity failed");
            }
        }
    }
    // The magnitude models, saved and re-read like the classifier.
    if (magnitude_source == "regression") {
        for (const auto& [file, booster] : {std::pair{"model_up.txt", &up_final}, std::pair{"model_down.txt", &down_final}}) {
            const std::string mpath = (tmp_dir / file).string();
            if (LGBM_BoosterSaveModel(booster->handle, 0, booster->best_iteration,
                                      C_API_FEATURE_IMPORTANCE_SPLIT, mpath.c_str()) != 0) {
                return fail(ErrorCode::internal_error, std::string("LGBM save: ") + LGBM_GetLastError());
            }
            std::vector<int64_t> sample(fte.begin(), fte.begin() + std::min<std::ptrdiff_t>(
                                                                     kSampleRows, static_cast<std::ptrdiff_t>(fte.size())));
            BoosterHandle re = nullptr;
            int re_iters = 0;
            if (LGBM_BoosterCreateFromModelfile(mpath.c_str(), &re_iters, &re) != 0) {
                return fail(ErrorCode::internal_error, std::string("saved magnitude model does not load: ") +
                                                           LGBM_GetLastError());
            }
            auto a = reg_predict(re, d, sample, -1);
            LGBM_BoosterFree(re);
            auto b = reg_predict(booster->handle, d, sample, booster->best_iteration);
            if (!a || !b) return fail(ErrorCode::internal_error, "magnitude parity prediction failed");
            for (size_t i = 0; i < a->size(); ++i) {
                if (std::abs((*a)[i] - (*b)[i]) > kSelfCheckTolerance) {
                    return fail(ErrorCode::validation_error, "magnitude write/read parity failed");
                }
            }
        }
    }
    const std::string names_sha = sha256_hex(recipe::joined_names(d.feature_names));
    const json calibration = {
        {"calibrator", econ::kCalibratorVersion},
        {"temperature", temperature},
        {"class_order", {"SHORT", "NEUTRAL", "LONG"}},
        {"class_means_return", {means.ret[0], means.ret[1], means.ret[2]}},
        {"class_means_mfe_long", {means.mfe[0], means.mfe[1], means.mfe[2]}},
        {"class_means_mae_long", {means.mae[0], means.mae[1], means.mae[2]}},
        {"class_counts_fit", {means.n[0], means.n[1], means.n[2]}},
        {"climatology_fit", {climate[0], climate[1], climate[2]}},
        {"horizon_minutes", hz.minutes},
        {"fee_model_version", fee_model_version},
        {"label_set", "econ-labels:1"},
        // The classes the model was fitted on are drawn against each row's
        // own cost-v2 estimate, not the flat figure stored in the label set.
        {"class_boundary", "per-row cost-v2 at T0"},
        {"cost_model", {{"version", econ::kCostModelVersion},
                        {"fee_bps_per_side", cost_model.fee_bps_per_side},
                        {"impact_bps_per_side", cost_model.impact_bps_per_side},
                        {"fallback_spread_bps", cost_model.fallback_spread_bps},
                        {"max_plausible_spread_bps", cost_model.max_plausible_spread_bps},
                        {"spread_source", "bitget_micro:book_spread_bps, latest window <= T0, "
                                          "computed before the target vector"}}},
        {"policy", {{"version", econ::kPolicyVersion},
                    {"margin_bps", policy.margin_bps ? json(*policy.margin_bps) : json(nullptr)},
                    {"margin_grid_bps", econ::kMarginGridBps},
                    {"min_validation_trades", econ::kMinValidationTrades},
                    {"validation_trades", policy.trades},
                    {"validation_net_bps", std::isfinite(policy.mean_net_bps) ? json(policy.mean_net_bps)
                                                                              : json(nullptr)},
                    {"chosen_on", "validation slice (tuned model, ftr class means)"},
                    {"strong_edge", "expected_net >= margin + round_trip_cost"}}},
        {"decision_rule", "DIAGNOSTIC ONLY (VPS1 decides nothing): side whose expected net "
                          "(p . class means -/+ cost at T0) exceeds the validated margin"},
        // What up / flat / down MEAN for this model.
        {"label_definition", "econ-class:cost-v2 -- up / down: the simple return over the horizon "
                             "beyond the round-trip cost-v2 estimated at T0; flat: within it"},
        {"calibration_version", econ::kCalibratorVersion},
        {"calibration_test", {{"version", econ::kCalibrationTestVersion},
                              {"passed", ctest.passed},
                              {"sims", ctest.sims},
                              {"ece", {ctest.ece[0], ctest.ece[1], ctest.ece[2]}},
                              {"null_q95", {ctest.null_q95[0], ctest.null_q95[1], ctest.null_q95[2]}},
                              {"slice", "holdout"}}},
        {"gates", "market-1"},
        {"magnitude_source", magnitude_source},
        {"magnitude", magnitude}};
    const json report = {
        {"trainer_version", kEconTrainerVersion},
        {"objective", objective},
        {"rows", rows.size()},
        {"features", d.n_features},
        {"sample_every_minutes", d.sample_minutes},
        {"recipe_version", cfg.recipe_version},
        {"walk_forward", folds_json},
        {"holdout", slice_json(holdout.score)},
        {"holdout_from", d.t_ms[static_cast<size_t>(fte.front())]},
        {"holdout_to", d.t_ms[static_cast<size_t>(fte.back())]},
        {"best_iterations", final_model.best_iteration},
        {"calibration", calibration},
        {"champion", champion_id.empty() ? json(nullptr) : json(champion_id)},
        {"champion_holdout_net_bps", std::isfinite(champion_net) ? json(champion_net * 1e4) : json(nullptr)},
        {"champion_holdout_logloss_skill", std::isfinite(champion_ll) ? json(champion_ll) : json(nullptr)},
        {"gates", "market-1"},
        {"gates_passed", gates_passed},
        {"gate_failures", gate_failures},
        {"econ_diagnostic_failures", econ_diagnostic},
        {"calibration_test_passed", ctest.passed},
        {"dataset_fingerprint", dataset_fingerprint},
        {"protocol", {{"timestamp_grouped", true},
                      {"embargo_minutes", embargo_ms / 60'000},
                      {"point_in_time", "source computed_at <= target computed_at; book spread by arrival"},
                      {"calibration_source", "validation slice preceding the scored slice"},
                      {"one_position_per_instrument", true},
                      {"cost_model", econ::kCostModelVersion},
                      {"policy", econ::kPolicyVersion}}}};
    {
        std::ofstream names(tmp_dir / "feature_names.json");
        json arr = json::array();
        for (const auto& n : d.feature_names) arr.push_back(n);
        names << arr.dump(1);
        std::ofstream cal(tmp_dir / "calibration.json");
        cal << calibration.dump(1);
        std::ofstream rep(tmp_dir / "report.json");
        rep << report.dump(1);
        // The holdout, row by row: what the model said and what happened.
        std::ofstream hp(tmp_dir / "holdout_predictions.csv");
        hp << "bar_open_ms,symbol,p_short,p_neutral,p_long,expected_net_long_bps,"
              "expected_net_short_bps,side,class,log_return,cost_bps,spread_observed\n";
        for (size_t i = 0; i < fte.size(); ++i) {
            const auto u = static_cast<size_t>(fte[i]);
            hp << d.t_ms[u] << ',' << d.symbols[static_cast<size_t>(d.sym_ids[u])] << ','
               << holdout.p[i * 3] << ',' << holdout.p[i * 3 + 1] << ',' << holdout.p[i * 3 + 2] << ','
               << holdout.est[i].expected_net_long * 1e4 << ','
               << holdout.est[i].expected_net_short * 1e4 << ',' << holdout.side[i] << ','
               << d.y[h][u] << ',' << d.r[h][u] << ',' << d.cost[u] * 1e4 << ','
               << static_cast<int>(d.spread_observed[u]) << '\n';
        }
        if (!names.good() || !cal.good() || !rep.good() || !hp.good()) {
            return fail(ErrorCode::internal_error, "cannot write model artifacts");
        }
    }
    fs::rename(tmp_dir, final_dir, fe);
    if (fe) return fail(ErrorCode::internal_error, "cannot publish model dir: " + fe.message());

    // ---- registry
    const int64_t t_lo = d.t_ms[static_cast<size_t>(ffit.front())];
    const int64_t t_hi = d.t_ms[static_cast<size_t>(ffit.back())] + 60'000;
    const int64_t h_lo = d.t_ms[static_cast<size_t>(fte.front())];
    const int64_t h_hi = d.t_ms[static_cast<size_t>(fte.back())] + 60'000;
    // The slice early stopping and the temperature were fitted on.
    const int64_t v_lo = d.t_ms[static_cast<size_t>(fva.front())];
    const int64_t v_hi = d.t_ms[static_cast<size_t>(fva.back())] + 60'000;
    auto reg = pool.with_txn([&](pqxx::work& txn) {
        txn.exec_params(
            "INSERT INTO altml.models (model_id, algorithm, objective, feature_set_version, "
            "label_set_version, dataset_id, hyperparameters, status, training_period, test_period, "
            "validation_period) "
            "VALUES ($1, 'lightgbm', $2, $3, 'econ-labels:1', $4, $5, 'CANDIDATE', "
            "tstzrange(to_timestamp($6::bigint / 1000.0), to_timestamp($7::bigint / 1000.0), '[)'), "
            "tstzrange(to_timestamp($8::bigint / 1000.0), to_timestamp($9::bigint / 1000.0), '[)'), "
            "tstzrange(to_timestamp($10::bigint / 1000.0), to_timestamp($11::bigint / 1000.0), '[)'))",
            model_id, objective,
            std::string(analytics::kCoreFeatureSetName) + ":" + std::to_string(analytics::kCoreFeatureSetVersion),
            // dataset_id: the data the model was fitted on -- source,
            // window, rows and a fingerprint of (time, instrument, label).
            "altml-archive:econ-labels:1:" + std::to_string(t_lo) + ".." + std::to_string(t_hi) +
                ":rows=" + std::to_string(ffit.size()) + ":sha=" + dataset_fingerprint,
            json({{"learning_rate", cfg.learning_rate}, {"num_leaves", cfg.num_leaves},
                  {"min_data_in_leaf", cfg.min_data_in_leaf}, {"feature_fraction", cfg.feature_fraction},
                  {"bagging_fraction", cfg.bagging_fraction}, {"lambda_l1", cfg.lambda_l1},
                  {"lambda_l2", cfg.lambda_l2}, {"max_depth", cfg.max_depth},
                  {"selected_iterations", final_model.best_iteration},
                  {"horizon_minutes", hz.minutes}, {"target", "econ_class_" + std::string(hz.name)},
                  {"cost_model", {{"version", econ::kCostModelVersion},
                                  {"fee_model_version", fee_model_version},
                                  {"fee_bps_per_side", cost_model.fee_bps_per_side},
                                  {"impact_bps_per_side", cost_model.impact_bps_per_side},
                                  {"fallback_spread_bps", cost_model.fallback_spread_bps}}},
                  {"policy", {{"version", econ::kPolicyVersion},
                              {"margin_bps", policy.margin_bps ? json(*policy.margin_bps) : json(nullptr)}}},
                  {"calibrator", econ::kCalibratorVersion},
                  {"calibration_version", econ::kCalibratorVersion},
                  {"calibration_test", {{"version", econ::kCalibrationTestVersion}, {"passed", ctest.passed}}},
                  {"label_definition", "econ-class:cost-v2"},
                  {"label_version", "econ-labels:1"},
                  {"gates", "market-1"},
                  {"magnitude_source", magnitude_source},
                  {"dataset_fingerprint", dataset_fingerprint},
                  {"recipe_version", cfg.recipe_version},
                  {"trainer_version", kEconTrainerVersion}})
                .dump(),
            t_lo, t_hi, h_lo, h_hi, v_lo, v_hi);
        for (const auto& [kind, file, format] :
             {std::tuple{"model", "model.txt", "lightgbm-text"},
              {"model_up", "model_up.txt", "lightgbm-text"},
              {"model_down", "model_down.txt", "lightgbm-text"},
              {"feature_names", "feature_names.json", "json"},
              {"calibration", "calibration.json", "json"},
              {"report", "report.json", "json"},
              {"holdout_predictions", "holdout_predictions.csv", "csv"}}) {
            const std::string path = (final_dir / file).string();
            if (!fs::exists(path)) continue;   // the magnitude models exist only when chosen
            txn.exec_params(
                "INSERT INTO altml.model_artifacts (model_id, kind, path, format, checksum, size_bytes) "
                "VALUES ($1, $2, $3, $4, $5, $6)",
                model_id, std::string(kind), path, std::string(format), file_sha256(path),
                static_cast<int64_t>(fs::file_size(path)));
        }
        const auto metric = [&](const std::string& scope, const std::string& name, double v) {
            if (!std::isfinite(v)) return;
            txn.exec_params("INSERT INTO altml.model_metrics (model_id, scope, metric, value) "
                            "VALUES ($1, $2, $3, $4) ON CONFLICT DO NOTHING",
                            model_id, scope, name, v);
        };
        metric("test", "calibration_test_passed", ctest.passed ? 1.0 : 0.0);
        for (const auto& key : {"regression_up_spearman", "regression_down_spearman", "class_means_up_spearman",
                                "class_means_down_spearman"}) {
            if (magnitude["holdout"].contains(key) && magnitude["holdout"][key].is_number())
                metric("test", std::string("magnitude_") + key, magnitude["holdout"][key].get<double>());
        }
        metric("test", "ece_neutral", ctest.ece[1]);
        for (size_t k = 0; k < 3; ++k) {
            static constexpr const char* kName[3] = {"ece_null_q95_short", "ece_null_q95_neutral",
                                                     "ece_null_q95_long"};
            metric("test", kName[k], ctest.null_q95[k]);
        }
        const auto slice_metrics = [&](const std::string& scope, const econ::SliceScore& s) {
            metric(scope, "logloss_skill", s.logloss_skill);
            metric(scope, "brier_skill", s.brier_skill);
            metric(scope, "direction_auc", s.direction_auc);
            metric(scope, "ece_long", s.ece_long);
            metric(scope, "ece_short", s.ece_short);
            metric(scope, "trades", static_cast<double>(s.econ.trades));
            metric(scope, "mean_net_bps", s.econ.mean_net * 1e4);
            metric(scope, "mean_gross_bps", s.econ.mean_gross * 1e4);
            metric(scope, "t_day", s.econ.t_day);
            metric(scope, "hit_rate", s.econ.hit_rate);
            metric(scope, "long_share", s.econ.long_share);
            metric(scope, "profit_factor", s.econ.profit_factor);
            metric(scope, "max_drawdown_units", s.econ.max_drawdown);
        };
        slice_metrics("test", holdout.score);
        for (size_t k = 0; k < fold_scores.size(); ++k) {
            slice_metrics("fold:" + std::to_string(k), fold_scores[k]);
            const auto tr = remap(folds[k].train, rows), va = remap(folds[k].valid, rows),
                       te = remap(folds[k].test, rows);
            txn.exec_params(
                "INSERT INTO altml.model_walk_forward_runs (model_id, fold, train_from, train_to, "
                "valid_from, valid_to, test_from, test_to, metrics) VALUES ($1, $2, "
                "to_timestamp($3::bigint / 1000.0), to_timestamp($4::bigint / 1000.0), "
                "to_timestamp($5::bigint / 1000.0), to_timestamp($6::bigint / 1000.0), "
                "to_timestamp($7::bigint / 1000.0), to_timestamp($8::bigint / 1000.0), $9)",
                model_id, static_cast<int>(k), d.t_ms[static_cast<size_t>(tr.front())],
                d.t_ms[static_cast<size_t>(tr.back())] + 60'000, d.t_ms[static_cast<size_t>(va.front())],
                d.t_ms[static_cast<size_t>(va.back())] + 60'000, d.t_ms[static_cast<size_t>(te.front())],
                d.t_ms[static_cast<size_t>(te.back())] + 60'000, folds_json[k]["metrics"].dump());
        }
        txn.exec_params(
            "INSERT INTO altml.model_feature_compatibility (model_id, feature_set_version, schema_hash, "
            "feature_count, base_feature_count, aligned_sources, aligned_projection, recipe_version, "
            "base_projection, feature_names_sha256, calibration) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11::jsonb)",
            model_id,
            std::string(analytics::kCoreFeatureSetName) + ":" + std::to_string(analytics::kCoreFeatureSetVersion),
            d.feature_schema_hash, d.n_features, d.base_width,
            training::pg_text_array_literal(d.source_specs),
            training::pg_text_array_literal(recipe::projection_for(cfg.recipe_version)),
            cfg.recipe_version, training::pg_text_array_literal(d.base_projection),
            names_sha, calibration.dump());
        txn.exec_params("INSERT INTO altml.model_events (model_id, event, detail, actor) "
                        "VALUES ($1, 'trained', $2, 'altml-trainer')",
                        model_id,
                        json({{"gates", "market-1"}, {"gates_passed", gates_passed},
                              {"gate_failures", gate_failures}, {"econ_diagnostic_failures", econ_diagnostic},
                              {"calibration_test_passed", ctest.passed}})
                            .dump());
    });
    if (!reg) return std::unexpected(reg.error());

    CycleOutcome outcome;
    outcome.model_id = model_id;
    if (!gates_passed) {
        auto rej = pool.with_txn([&](pqxx::work& txn) {
            txn.exec_params("UPDATE altml.models SET status = 'REJECTED' WHERE model_id = $1", model_id);
            txn.exec_params("INSERT INTO altml.model_events (model_id, event, detail, actor) "
                            "VALUES ($1, 'rejected', $2, 'altml-trainer')",
                            model_id, json({{"gate_failures", gate_failures}}).dump());
        });
        if (!rej) return std::unexpected(rej.error());
        outcome.status = "REJECTED";
        outcome.detail = gate_failures.front();
        return outcome;
    }
    if (!cfg.auto_promote) {
        auto appr = pool.with_txn([&](pqxx::work& txn) {
            txn.exec_params("UPDATE altml.models SET status = 'APPROVED', approved_at = now() "
                            "WHERE model_id = $1", model_id);
        });
        if (!appr) return std::unexpected(appr.error());
        outcome.status = "APPROVED";
        outcome.detail = "gates passed; waiting for operator activation";
        return outcome;
    }
    auto promo = pool.with_txn([&](pqxx::work& txn) {
        const auto locked = txn.exec_params(
            "SELECT status FROM altml.models WHERE model_id = $1 FOR UPDATE", model_id);
        if (locked.empty() || locked[0][0].as<std::string>() != "CANDIDATE") {
            throw std::runtime_error("candidate status changed during promotion");
        }
        const auto prev = txn.exec_params(
            "SELECT model_id FROM altml.models WHERE status = 'ACTIVE' AND objective = $1 FOR UPDATE",
            objective);
        std::string prev_id;
        if (!prev.empty()) {
            prev_id = prev[0][0].as<std::string>();
            txn.exec_params("UPDATE altml.models SET status = 'RETIRED', retired_at = now() "
                            "WHERE model_id = $1", prev_id);
            txn.exec_params("INSERT INTO altml.model_events (model_id, event, detail, actor) "
                            "VALUES ($1, 'retired', $2, 'altml-trainer')",
                            prev_id, json({{"replaced_by", model_id}}).dump());
        }
        txn.exec_params("UPDATE altml.models SET status = 'ACTIVE', approved_at = now(), "
                        "activated_at = now(), retired_at = NULL WHERE model_id = $1", model_id);
        txn.exec_params("INSERT INTO altml.model_events (model_id, event, detail, actor) "
                        "VALUES ($1, 'activated', $2, 'altml-trainer')",
                        model_id, json({{"previous", prev_id}}).dump());
    });
    if (!promo) return std::unexpected(promo.error());
    outcome.status = "ACTIVE";
    outcome.detail = "promoted";
    return outcome;
}

// One econ cycle: load once, train every configured horizon.
Result<std::vector<CycleOutcome>> run_econ_cycle(persistence::PgPool& pool,
                                                 const config::RuntimeConfig& rt,
                                                 const TrainerConfig& cfg) {
    auto labels = resolve_econ_labels(pool);
    if (!labels) return std::unexpected(labels.error());
    double taker = -1, slip = -1;
    if (auto r = pool.with_txn([&](pqxx::work& txn) {
            const auto row = txn.exec_params(
                "SELECT taker_fee::float8, slippage_estimate_bps::float8 FROM altml.fee_models "
                "WHERE version = $1", labels->fee_model_version);
            if (!row.empty()) { taker = row[0][0].as<double>(); slip = row[0][1].as<double>(); }
        });
        !r) {
        return std::unexpected(r.error());
    }
    // The fee is the fee model's; the spread is what the book showed at each
    // T0 (cost model v2); impact and the no-reading fallback are configured.
    if (taker < 0 || slip < 0) return fail(ErrorCode::validation_error, "econ label fee model missing");
    econ::CostModelV2 cost;
    cost.fee_bps_per_side = taker * 10'000.0;
    cost.impact_bps_per_side = cfg.econ_impact_bps_per_side;
    cost.fallback_spread_bps = cfg.econ_fallback_spread_bps;

    const auto started = std::chrono::steady_clock::now();
    auto data = load_econ_data(pool, cfg, *labels, rt.project.exchange, cost);
    if (!data) return std::unexpected(data.error());
    log::info("econ_training_data_loaded",
              {{"rows", static_cast<int64_t>(data->rows())},
               {"features", static_cast<int64_t>(data->n_features)},
               {"load_seconds", std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::steady_clock::now() - started).count()}});
    std::vector<CycleOutcome> out;
    for (const auto& name : cfg.econ_horizons) {
        size_t h = 0;
        while (h < econ::kHorizons.size() && econ::kHorizons[h].name != name) ++h;
        if (h == econ::kHorizons.size()) return fail(ErrorCode::config_error, "unknown horizon " + name);
        auto o = run_econ_horizon(pool, rt, cfg, *data, h, cost, labels->fee_model_version);
        if (!o) {
            if (o.error().code == ErrorCode::shutdown_requested) return std::unexpected(o.error());
            CycleOutcome failed;
            failed.detail = name + ": " + o.error().message;
            // Not enough labeled history yet is a DEFERRAL -- the labels are
            // still being built -- not a failure of the model or the data, and
            // it must not read like one in the logs or feed the rejection
            // backoff.
            if (o.error().retryable) {
                failed.status = "DEFERRED";
                log::info("econ_horizon_deferred", {{"horizon", name}, {"reason", o.error().message}});
            } else {
                failed.status = "FAILED";
                log::error("econ_horizon_failed", {{"horizon", name}, {"message", o.error().message}});
            }
            out.push_back(failed);
            continue;
        }
        log::info("econ_horizon_done", {{"horizon", name}, {"model_id", o->model_id},
                                        {"status", o->status}, {"detail", o->detail}});
        out.push_back(*o);
    }
    return out;
}

// ---------------------------------------------------------------- scheduling

Result<bool> has_active_model(persistence::PgPool& pool, const std::string& objective) {
    bool active = false;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT EXISTS (SELECT 1 FROM altml.models WHERE status = 'ACTIVE' "
            "AND algorithm = 'lightgbm' AND objective = $1)",
            objective);
        active = rows[0][0].as<bool>();
    });
    if (!res) return std::unexpected(res.error());
    return active;
}

Result<int64_t> last_training_ms(persistence::PgPool& pool, const std::string& objective) {
    int64_t out = 0;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT (extract(epoch FROM max(created_at)) * 1000)::bigint FROM altml.models "
            "WHERE algorithm = 'lightgbm' AND objective = $1 AND status <> 'FAILED'",
            objective);
        if (!rows.empty() && !rows[0][0].is_null()) out = rows[0][0].as<int64_t>();
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

// How many models in a row, most recent first, ended REJECTED -- i.e. every
// model trained since the last one that was not rejected. Read from the
// database rather than kept only in memory, because a counter that resets on
// restart resets the backoff too: a deploy, or a crash loop, would put the
// hourly retrain right back.
Result<int> trailing_rejections(persistence::PgPool& pool, const std::string& objective) {
    int out = 0;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec_params(
            "SELECT count(*)::int FROM altml.models "
            "WHERE algorithm = 'lightgbm' AND objective = $1 AND status <> 'FAILED' "
            "  AND created_at > COALESCE((SELECT max(created_at) FROM altml.models "
            "      WHERE algorithm = 'lightgbm' AND objective = $1 "
            "        AND status NOT IN ('REJECTED', 'FAILED')), '-infinity'::timestamptz)",
            objective);
        if (!rows.empty() && !rows[0][0].is_null()) out = rows[0][0].as<int>();
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

int64_t utc_hour(int64_t ms) { return (ms / 3'600'000) % 24; }

} // namespace

// ------------------------------------------------------------- the component

struct TrainerComponent::Impl {
    TrainerConfig cfg;
    std::optional<persistence::PgPool> pool;
    int64_t last_attempt_ms = 0;
    // How many training cycles in a row ended in REJECTED. Drives the backoff
    // below; reset the moment a cycle produces an APPROVED or ACTIVE model.
    int consecutive_rejections = 0;
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
    // Resolve cost from its canonical home unless the config pinned it.
    //
    // altml.fee_models is the same table the label builder reads, so the
    // economic gate and the `future_return_after_fees_*` labels price a trade
    // with one set of numbers. A config that states the values explicitly still
    // wins -- an operator overriding a measured cost is a deliberate act -- but
    // silence now means "use the canonical model", not "use a literal that was
    // typed here once".
    if (!impl_->cfg.cost_from_config) {
        double taker = -1.0, slip = -1.0;
        auto read = impl_->pool->with_txn([&](pqxx::work& txn) {
            const auto r = txn.exec_params(
                "SELECT taker_fee::float8, slippage_estimate_bps::float8 "
                "FROM altml.fee_models WHERE version = $1",
                impl_->cfg.fee_model_version);
            if (!r.empty()) {
                taker = r[0][0].as<double>();
                slip = r[0][1].as<double>();
            }
        });
        if (read && taker >= 0.0 && slip >= 0.0) {
            const auto cost = training::round_trip_from_fee_model(taker, slip);
            impl_->cfg.round_trip_fee_bps = cost.fee_bps;
            impl_->cfg.round_trip_slippage_bps = cost.slippage_bps;
            log::info("trainer_cost_model_resolved",
                      {{"fee_model_version", impl_->cfg.fee_model_version},
                       {"round_trip_fee_bps", impl_->cfg.round_trip_fee_bps},
                       {"round_trip_slippage_bps", impl_->cfg.round_trip_slippage_bps}});
        } else {
            // Never silently fall back to a literal: a missing cost model is a
            // configuration error, and pricing an edge against a guess is worse
            // than not training.
            return fail(ErrorCode::validation_error,
                        "fee model version " + std::to_string(impl_->cfg.fee_model_version) +
                            " not found in altml.fee_models and no explicit cost in config");
        }
    }

    // The feature/label catalog must be resolvable at startup; actual history
    // sufficiency is re-checked before every cycle (it grows over time).
    //
    // In econ mode the label set is created by the label builder, which may
    // initialise after this component on a fresh database; its absence is
    // then a deferral reported by the cycle, not a start-up failure.
    if (impl_->cfg.enabled && impl_->cfg.mode == "econ") {
        if (auto labels = resolve_econ_labels(*impl_->pool); !labels) {
            log::warn("econ_label_catalog_pending", {{"message", labels.error().message}});
        }
    } else if (impl_->cfg.enabled) {
        if (auto sets = resolve_sets(*impl_->pool, impl_->cfg); !sets) {
            return fail(sets.error().code,
                        "training catalog invalid: " + sets.error().message);
        }
    }
    set_component_status(name(), impl_->cfg.enabled ? "scheduling" : "disabled");
    return {};
}

void TrainerComponent::run() {
    auto& shutdown = ShutdownToken::instance();
    auto& cfg = impl_->cfg;
    auto& pool = *impl_->pool;
    if (!cfg.enabled) {
        while (!shutdown.stop_requested()) shutdown.wait_for(std::chrono::seconds(60));
        return;
    }
    if (cfg.mode == "econ") {
        run_econ_schedule();
        return;
    }
    // Recover the schedule from the database so a restart resumes the backoff
    // instead of restarting it. Without this the process trains on every start.
    if (auto rejections = trailing_rejections(pool, objective_of(cfg))) {
        impl_->consecutive_rejections = *rejections;
    } else {
        log::warn("trainer_backoff_recovery_failed", {{"message", rejections.error().message}});
    }
    if (auto seeded = last_training_ms(pool, objective_of(cfg)); seeded && *seeded > 0) {
        impl_->last_attempt_ms = *seeded;
    }
    log::info("trainer_schedule_recovered",
              {{"consecutive_rejections", impl_->consecutive_rejections},
               {"last_attempt_ms", impl_->last_attempt_ms}});

    while (!shutdown.stop_requested()) {
        const int64_t now = timeutil::now_ms();
        bool due = false;
        std::string why;

        auto active = has_active_model(pool, objective_of(cfg));
        auto last_train = last_training_ms(pool, objective_of(cfg));
        if (active && last_train) {
            // Back off after repeated rejections instead of retrying flat forever.
            //
            // `bootstrap_if_no_active` re-arms on every pass while there is no
            // ACTIVE model, so a flat retry means: train, fail the gates, wait
            // an hour, train again on one more hour of history, fail again --
            // for as long as the objective stays unwinnable. Thirty-seven
            // rejected models were produced that way, and it was not free: each
            // cycle spiked a four-core box with exhausted swap, stretching
            // AltTech's cross-venue sweep past its watchdog budget. Eighteen
            // trainings in a day lined up with eighteen holes in the feature
            // vector coverage, one per run.
            //
            // An hour of new bars cannot overturn a verdict measured over
            // months, so the retry buys nothing and costs the live path. Double
            // the wait per consecutive rejection, capped at the scheduled
            // interval, so genuinely new data still gets its chance on the
            // normal cadence. Nothing here weakens a gate or promotes anything:
            // a rejected model stays rejected, and NO ACTIVE MODEL stays the
            // honest answer.
            int64_t retry_ms = cfg.retry_minutes * 60'000;
            for (int i = 0; i < impl_->consecutive_rejections && i < 8; ++i) retry_ms *= 2;
            if (cfg.interval_hours > 0) {
                retry_ms = std::min(retry_ms, cfg.interval_hours * 3'600'000);
            }
            const bool retry_ok = now - impl_->last_attempt_ms >= retry_ms;
            if (!*active && cfg.bootstrap_if_no_active && retry_ok) {
                due = true;
                why = "bootstrap";
            } else if (cfg.interval_hours > 0 &&
                       now - *last_train >= cfg.interval_hours * 3'600'000 && retry_ok &&
                       (cfg.start_hour_utc < 0 || utc_hour(now) == cfg.start_hour_utc)) {
                due = true;
                why = "scheduled";
            }
        } else {
            log::warn("trainer_schedule_check_failed",
                      {{"message", (!active ? active.error() : last_train.error()).message}});
        }

        if (due) {
            if (const auto pressure = resource_pressure(cfg)) {
                // Live inference keeps priority; retry after the backoff.
                impl_->last_attempt_ms = now;
                set_component_status(name(), "JOB_DEFERRED_RESOURCE_PRESSURE");
                log::warn("JOB_DEFERRED_RESOURCE_PRESSURE", {{"reason", *pressure}});
            } else {
                impl_->last_attempt_ms = now;
                set_component_status(name(), "training (" + why + ")");
                log::info("training_cycle_started", {{"reason", why}});
                auto outcome = run_cycle(pool, rt_, cfg);
                if (outcome) {
                    if (outcome->status == "REJECTED") {
                        ++impl_->consecutive_rejections;
                    } else {
                        impl_->consecutive_rejections = 0;
                    }
                    set_component_status(name(), "last=" + outcome->status);
                    log::info("training_cycle_done", {{"model_id", outcome->model_id},
                                                      {"status", outcome->status},
                                                      {"detail", outcome->detail}});
                } else if (outcome.error().code == ErrorCode::shutdown_requested) {
                    break;
                } else {
                    set_component_status(name(), "last=FAILED");
                    log::error("training_cycle_failed",
                               {{"message", outcome.error().message}});
                }
            }
        } else {
            set_component_status(name(), "idle");
        }
        if (shutdown.wait_for(std::chrono::seconds(60))) break;
    }
}

// The econ schedule: one cycle trains every configured horizon from one data
// load. Due when a horizon has no ACTIVE model (bootstrap, with the same
// rejection backoff as the legacy path, driven by the most-rejected horizon)
// or when the weekly interval has passed.
void TrainerComponent::run_econ_schedule() {
    auto& shutdown = ShutdownToken::instance();
    auto& cfg = impl_->cfg;
    auto& pool = *impl_->pool;
    std::vector<std::string> objectives;
    for (const auto& h : cfg.econ_horizons) objectives.push_back(econ::objective_of(h));
    const auto recover = [&]() {
        int worst = 0;
        int64_t last = 0;
        for (const auto& o : objectives) {
            if (auto r = trailing_rejections(pool, o)) worst = std::max(worst, *r);
            if (auto l = last_training_ms(pool, o); l && *l > last) last = *l;
        }
        impl_->consecutive_rejections = worst;
        if (last > 0) impl_->last_attempt_ms = last;
    };
    recover();
    log::info("trainer_schedule_recovered", {{"mode", "econ"},
                                             {"consecutive_rejections", impl_->consecutive_rejections},
                                             {"last_attempt_ms", impl_->last_attempt_ms}});
    while (!shutdown.stop_requested()) {
        const int64_t now = timeutil::now_ms();
        bool all_active = true, check_ok = true;
        int64_t last_train = 0;
        for (const auto& o : objectives) {
            auto a = has_active_model(pool, o);
            auto l = last_training_ms(pool, o);
            if (!a || !l) { check_ok = false; break; }
            all_active = all_active && *a;
            last_train = std::max(last_train, *l);
        }
        bool due = false;
        std::string why;
        if (check_ok) {
            int64_t retry_ms = cfg.retry_minutes * 60'000;
            for (int i = 0; i < impl_->consecutive_rejections && i < 8; ++i) retry_ms *= 2;
            if (cfg.interval_hours > 0) retry_ms = std::min(retry_ms, cfg.interval_hours * 3'600'000);
            const bool retry_ok = now - impl_->last_attempt_ms >= retry_ms;
            if (!all_active && cfg.bootstrap_if_no_active && retry_ok) {
                due = true;
                why = "bootstrap";
            } else if (cfg.interval_hours > 0 && now - last_train >= cfg.interval_hours * 3'600'000 &&
                       retry_ok && (cfg.start_hour_utc < 0 || utc_hour(now) == cfg.start_hour_utc)) {
                due = true;
                why = "scheduled";
            }
        } else {
            log::warn("trainer_schedule_check_failed", {{"mode", "econ"}});
        }
        if (due) {
            impl_->last_attempt_ms = now;
            if (const auto pressure = resource_pressure(cfg)) {
                set_component_status(name(), "JOB_DEFERRED_RESOURCE_PRESSURE");
                log::warn("JOB_DEFERRED_RESOURCE_PRESSURE", {{"reason", *pressure}});
            } else {
                set_component_status(name(), "training econ (" + why + ")");
                log::info("training_cycle_started", {{"reason", why}, {"mode", "econ"}});
                auto outcomes = run_econ_cycle(pool, rt_, cfg);
                if (outcomes) {
                    bool any_accepted = false, any_rejected = false;
                    std::string summary;
                    for (const auto& o : *outcomes) {
                        if (o.status == "ACTIVE" || o.status == "APPROVED") any_accepted = true;
                        if (o.status == "REJECTED") any_rejected = true;
                        if (!summary.empty()) summary += ",";
                        summary += o.status;
                    }
                    // Only a real verdict moves the backoff: a deferral (history
                    // still accumulating) retries on the base cadence.
                    if (any_accepted) impl_->consecutive_rejections = 0;
                    else if (any_rejected) ++impl_->consecutive_rejections;
                    set_component_status(name(), "last=" + summary);
                } else if (outcomes.error().code == ErrorCode::shutdown_requested) {
                    break;
                } else {
                    set_component_status(name(), "last=FAILED");
                    log::error("training_cycle_failed", {{"mode", "econ"},
                                                         {"message", outcomes.error().message}});
                }
            }
        } else {
            set_component_status(name(), "idle");
        }
        if (shutdown.wait_for(std::chrono::seconds(60))) break;
    }
}


// ------------------------------------------------------------------ replay
//
// Deterministic replay of a model's LIVE predictions: for every row the model
// wrote to altml.model_predictions, rebuild its input from the archive under
// the trainer's point-in-time rule, score it with the registered artifact and
// calibration, and compare. Two things are proven at once: the served input is
// the trained input (train/serve parity, including which aligned sources were
// available), and the same input with the same model gives the same output
// (the booster is scored twice and must agree bit for bit).
int run_prediction_replay(const config::RuntimeConfig& rt, const std::string& model_id,
                          int64_t from_ms, int64_t to_ms, int64_t limit) {
    persistence::PgPool pool(rt.common.postgres_dsn, 1);
    std::string path, checksum, hash, recipe_version, names_sha, calibration_text, objective;
    int fcount = 0, base_count = 0;
    std::vector<std::string> specs, base_projection;
    std::vector<std::string> core_layout;
    auto r = pool.with_txn([&](pqxx::work& txn) {
        const auto row = txn.exec_params(
            "SELECT a.path, a.checksum, c.schema_hash, c.feature_count, c.base_feature_count, "
            "       c.aligned_sources::text, c.base_projection::text, c.recipe_version, "
            "       c.feature_names_sha256, c.calibration::text, m.objective "
            "FROM altml.models m "
            "JOIN altml.model_artifacts a ON a.model_id = m.model_id AND a.kind = 'model' "
            "JOIN altml.model_feature_compatibility c ON c.model_id = m.model_id "
            "WHERE m.model_id = $1",
            model_id);
        if (row.empty()) return;
        path = row[0][0].as<std::string>();
        checksum = row[0][1].as<std::string>();
        hash = row[0][2].as<std::string>();
        fcount = row[0][3].as<int>();
        base_count = row[0][4].as<int>();
        specs = training::parse_pg_text_array(row[0][5].as<std::string>());
        base_projection = training::parse_pg_text_array(row[0][6].as<std::string>());
        recipe_version = row[0][7].as<std::string>();
        names_sha = row[0][8].as<std::string>();
        calibration_text = row[0][9].as<std::string>();
        objective = row[0][10].as<std::string>();
        for (const auto& m : txn.exec_params(
                 "SELECT fm.output_name FROM altml.fdw_feature_set_members fm "
                 "JOIN altml.fdw_feature_set_versions fv ON fv.id = fm.feature_set_version_id "
                 "WHERE fv.schema_hash = $1 ORDER BY fm.position",
                 hash)) {
            core_layout.push_back(m[0].as<std::string>());
        }
    });
    if (!r) { std::fprintf(stderr, "registry read failed: %s\n", r.error().message.c_str()); return 1; }
    if (path.empty()) { std::fprintf(stderr, "no such model: %s\n", model_id.c_str()); return 2; }
    if (recipe::projection_for(recipe_version).empty()) { std::fprintf(stderr, "not an econ model\n"); return 2; }
    {
        // Registry paths are data, not authority -- the same rule inference
        // applies: the artifact must resolve inside the configured model root.
        std::error_code e1, e2;
        const auto root = fs::weakly_canonical(rt.project.model_root, e1);
        const auto art = fs::canonical(path, e2);
        const auto rel = e1 || e2 ? fs::path() : art.lexically_relative(root);
        if (rel.empty() || *rel.begin() == "..") {
            std::fprintf(stderr, "artifact outside the model root\n");
            return 1;
        }
        path = art.string();
    }
    if (file_sha256(path) != checksum) { std::fprintf(stderr, "artifact checksum mismatch\n"); return 1; }
    econ::CalibrationError cal_error;
    const auto cal = econ::parse_calibration(json::parse(calibration_text, nullptr, false), &cal_error);
    if (!cal) { std::fprintf(stderr, "calibration refused: %s\n", cal_error.reason.c_str()); return 1; }

    // Recipe, exactly as the trainer built it.
    std::vector<size_t> base_pos;
    std::vector<std::string> names;
    for (const auto& n : base_projection) {
        const auto it = std::find(core_layout.begin(), core_layout.end(), n);
        if (it == core_layout.end()) { std::fprintf(stderr, "base column missing: %s\n", n.c_str()); return 1; }
        base_pos.push_back(static_cast<size_t>(it - core_layout.begin()));
    }
    recipe::append_base_names(base_projection, names);
    const auto base_derived = recipe::DerivedRecipe::resolve(core_layout);
    std::vector<EconSource> sources;
    for (size_t i = 0; i < specs.size(); ++i) {
        const auto parsed = training::parse_aligned_source(specs[i], i);
        if (!parsed) { std::fprintf(stderr, "bad source %s\n", specs[i].c_str()); return 1; }
        EconSource es;
        es.src = *parsed;
        if (!recipe::source_method_compatible(recipe_version, es.src.feature_set, es.src.engine_version)) {
            std::fprintf(stderr, "source method unpinned or incompatible\n"); return 1;
        }
        if (es.src.feature_set == analytics::kCoreFeatureSetName) {
            es.src.equivalent_versions = {analytics::kCoreFeatureSetVersion};
            es.src.published_width = core_layout.size();
            for (const auto& n : recipe::projection_for(recipe_version)) {
                const auto it = std::find(core_layout.begin(), core_layout.end(), n);
                if (it == core_layout.end()) { std::fprintf(stderr, "source column missing: %s\n", n.c_str()); return 1; }
                es.src.projection.push_back(n);
                es.src.positions.push_back(static_cast<size_t>(it - core_layout.begin()));
            }
            es.derived = true;
            es.recipe = recipe::DerivedRecipe::resolve(core_layout);
        } else {
            auto layout = load_set_layout(pool, es.src.feature_set, es.src.feature_set_version);
            if (!layout) { std::fprintf(stderr, "layout: %s\n", layout.error().message.c_str()); return 1; }
            if (!recipe::source_layout_compatible(es.src.feature_set, es.src.feature_set_version, layout->columns)) {
                std::fprintf(stderr, "unsupported source layout\n"); return 1;
            }
            es.src.equivalent_versions = layout->equivalent_versions;
            es.src.published_width = layout->columns.size();
            for (size_t k = 0; k < layout->columns.size(); ++k) {
                if (recipe::is_excluded(es.src.feature_set, layout->columns[k])) continue;
                es.src.projection.push_back(layout->columns[k]);
                es.src.positions.push_back(k);
            }
        }
        recipe::append_source_names(
            recipe::source_prefix(es.src.exchange_id, es.src.timeframe, es.src.feature_set),
            es.src.projection, es.derived, names);
        sources.push_back(std::move(es));
    }
    if (static_cast<int>(names.size()) != fcount ||
        sha256_hex(recipe::joined_names(names)) != names_sha) {
        std::fprintf(stderr, "recipe does not rebuild the registered feature space\n");
        return 1;
    }
    BoosterHandle booster = nullptr;
    int iters = 0;
    if (LGBM_BoosterCreateFromModelfile(path.c_str(), &iters, &booster) != 0) {
        std::fprintf(stderr, "model load failed\n");
        return 1;
    }
    struct Free { BoosterHandle b; ~Free() { if (b) LGBM_BoosterFree(b); } } guard{booster};
    // The magnitude regressions, when the model serves them: same trust rules.
    BoosterHandle up_b = nullptr, down_b = nullptr;
    if (cal->magnitude_source == "regression") {
        std::map<std::string, std::pair<std::string, std::string>> arts;
        auto ar = pool.with_txn([&](pqxx::work& txn) {
            for (const auto& r : txn.exec_params("SELECT kind, path, checksum FROM altml.model_artifacts "
                                                 "WHERE model_id = $1 AND kind IN ('model_up','model_down')",
                                                 model_id)) {
                arts[r[0].as<std::string>()] = {r[1].as<std::string>(), r[2].as<std::string>()};
            }
        });
        if (!ar || arts.size() != 2) { std::fprintf(stderr, "magnitude artifacts missing\n"); return 1; }
        std::error_code e1;
        const auto root = fs::weakly_canonical(rt.project.model_root, e1);
        for (auto [kind, handle] : {std::pair{"model_up", &up_b}, std::pair{"model_down", &down_b}}) {
            std::error_code e2;
            const auto art = fs::canonical(arts.at(kind).first, e2);
            const auto rel = e1 || e2 ? fs::path() : art.lexically_relative(root);
            if (rel.empty() || *rel.begin() == ".." || file_sha256(art.string()) != arts.at(kind).second) {
                std::fprintf(stderr, "magnitude artifact untrusted\n");
                return 1;
            }
            int it = 0;
            if (LGBM_BoosterCreateFromModelfile(art.string().c_str(), &it, handle) != 0) {
                std::fprintf(stderr, "magnitude model load failed\n");
                return 1;
            }
        }
    }
    Free guard_up{up_b}, guard_down{down_b};

    auto spread_pos = spread_positions(pool);
    if (!spread_pos) { std::fprintf(stderr, "registry read failed: %s\n", spread_pos.error().message.c_str()); return 1; }
    std::string cols = "SELECT f.values::text, f.validity_bitmap::text, cs.spread::float8";
    std::string joins = spread_lateral_sql(*spread_pos);
    for (const auto& es : sources) {
        cols += ", " + es.src.alias + ".values::text, " + es.src.alias + ".bmp::text";
        joins += training::aligned_source_sql_pit(es.src);
    }
    const std::string input_sql =
        cols + " FROM altml.econ_feature_vectors f " + joins +
        "WHERE f.exchange_id = 1 AND f.instrument_id = $1 AND f.timeframe = '1m' "
        "AND f.bar_open_time = to_timestamp($2::bigint / 1000.0) AND f.bar_revision = $3 "
        "AND f.schema_hash = $4 AND f.feature_set_name = 'talib-core' "
        "ORDER BY f.computed_at LIMIT 1";
    constexpr int kFirstSource = 3;

    // The whole live chain, replayed from the archive: features -> model ->
    // calibration -> cost at T0 -> estimate -> policy decision. Each stage is
    // compared against what the live process stored for the same bar.
    int64_t compared = 0, no_input = 0, exact = 0, within_1e4 = 0, missing_differs = 0,
            nondeterministic = 0;
    int64_t cost_rows = 0, cost_version_differs = 0, margin_differs = 0, decision_differs = 0,
            live_actionable = 0, spread_archive_match = 0, spread_archive_differs = 0, magnitude_rows = 0;
    double max_diff = 0.0, max_net_diff_bps = 0.0, max_mag_diff_bps = 0.0;
    auto q = pool.with_txn([&](pqxx::work& txn) {
        txn.exec("SET LOCAL statement_timeout = '600s'");
        const auto live = txn.exec_params(
            "SELECT instrument_id, (extract(epoch FROM bar_open_time) * 1000)::bigint, bar_revision, "
            "       p_short, p_neutral, p_long, aligned_missing, cost_model_version, spread_bps, "
            "       spread_observed, policy_margin_bps, expected_net_long_bps, expected_net_short_bps, "
            "       expected_up_excursion_bps, expected_down_excursion_bps "
            "FROM altml.model_predictions WHERE model_id = $1 "
            "AND bar_open_time >= to_timestamp($2::bigint / 1000.0) "
            "AND bar_open_time < to_timestamp($3::bigint / 1000.0) "
            "ORDER BY bar_open_time, instrument_id LIMIT $4",
            model_id, from_ms, to_ms, limit);
        std::vector<float> derived(recipe::DerivedRecipe::width());
        for (const auto& row : live) {
            const auto in = txn.exec_params(input_sql, row[0].as<int64_t>(), row[1].as<int64_t>(),
                                            row[2].as<int64_t>(), hash);
            if (in.empty()) { ++no_input; continue; }
            const auto full = with_validity(training::parse_pg_float_array(in[0][0].as<std::string>()),
                                            training::parse_pg_hex_bytea(in[0][1].as<std::string>()));
            std::vector<double> x;
            for (const size_t p : base_pos) x.push_back(full[p]);
            base_derived.compute(full.data(), full.size(), derived.data());
            for (const float v : derived) x.push_back(v);
            int missing = 0;
            for (size_t si = 0; si < sources.size(); ++si) {
                const auto& es = sources[si];
                const auto& vc = in[0][static_cast<pqxx::row::size_type>(kFirstSource + 2 * si)];
                const auto& bc = in[0][static_cast<pqxx::row::size_type>(kFirstSource + 1 + 2 * si)];
                const size_t width = es.src.positions.size() + (es.derived ? derived.size() : 0);
                if (vc.is_null() || bc.is_null()) {
                    ++missing;
                    x.insert(x.end(), width, std::nan(""));
                    continue;
                }
                const auto sfull = with_validity(training::parse_pg_float_array(vc.as<std::string>()),
                                                 training::parse_pg_hex_bytea(bc.as<std::string>()));
                for (const size_t p : es.src.positions) x.push_back(p < sfull.size() ? sfull[p] : std::nan(""));
                if (es.derived) {
                    es.recipe.compute(sfull.data(), sfull.size(), derived.data());
                    for (const float v : derived) x.push_back(v);
                }
            }
            double raw1[3], raw2[3];
            int64_t len = 0;
            LGBM_BoosterPredictForMatSingleRow(booster, x.data(), C_API_DTYPE_FLOAT64,
                                               static_cast<int32_t>(x.size()), 1,
                                               C_API_PREDICT_RAW_SCORE, 0, -1, "", &len, raw1);
            LGBM_BoosterPredictForMatSingleRow(booster, x.data(), C_API_DTYPE_FLOAT64,
                                               static_cast<int32_t>(x.size()), 1,
                                               C_API_PREDICT_RAW_SCORE, 0, -1, "", &len, raw2);
            if (std::memcmp(raw1, raw2, sizeof(raw1)) != 0) ++nondeterministic;
            const auto p = econ::softmax3(raw1, cal->temperature);
            const double live_p[3] = {row[3].as<double>(), row[4].as<double>(), row[5].as<double>()};
            double d = 0;
            for (size_t k = 0; k < 3; ++k) d = std::max(d, std::abs(p[k] - live_p[k]));
            max_diff = std::max(max_diff, d);
            ++compared;
            if (d <= 1e-6) ++exact;          // REAL storage: ~7 significant digits
            if (d <= 1e-4) ++within_1e4;
            if (missing != row[6].as<int>()) ++missing_differs;

            // Rows stored before cost v2 carry no cost inputs to replay from.
            if (row[7].is_null() || row[8].is_null() || row[9].is_null()) continue;
            ++cost_rows;
            if (row[7].as<std::string>() != econ::kCostModelVersion) ++cost_version_differs;
            // The spread the live process saw is an input, recorded; the
            // archive's point-in-time reading of it is checked separately.
            const bool observed = row[9].as<bool>();
            const double live_spread = row[8].as<double>();
            const auto cost = cal->cost_model.at(observed ? live_spread : std::nan(""));
            const auto est = econ::estimate(p, cal->means, cost.fraction());
            // The published magnitudes, recomputed the way the model serves them.
            if (!row[13].is_null() && !row[14].is_null()) {
                double up = std::max(est.expected_mfe_long * 10'000.0, 0.0);
                double dn = std::max(-est.expected_mae_long * 10'000.0, 0.0);
                if (up_b != nullptr && down_b != nullptr) {
                    double yu = 0, yd = 0;
                    int64_t lu = 0, ld = 0;
                    LGBM_BoosterPredictForMatSingleRow(up_b, x.data(), C_API_DTYPE_FLOAT64,
                                                       static_cast<int32_t>(x.size()), 1, C_API_PREDICT_NORMAL,
                                                       0, -1, "", &lu, &yu);
                    LGBM_BoosterPredictForMatSingleRow(down_b, x.data(), C_API_DTYPE_FLOAT64,
                                                       static_cast<int32_t>(x.size()), 1, C_API_PREDICT_NORMAL,
                                                       0, -1, "", &ld, &yd);
                    up = std::max(std::expm1(yu), 0.0);
                    dn = std::max(std::expm1(yd), 0.0);
                }
                ++magnitude_rows;
                // REAL storage: ~7 significant digits of a value in the tens of bps.
                max_mag_diff_bps = std::max({max_mag_diff_bps, std::abs(up - row[13].as<double>()),
                                             std::abs(dn - row[14].as<double>())});
            }
            const double live_net_long = row[11].as<double>(), live_net_short = row[12].as<double>();
            max_net_diff_bps = std::max({max_net_diff_bps,
                                         std::abs(est.expected_net_long * 10'000.0 - live_net_long),
                                         std::abs(est.expected_net_short * 10'000.0 - live_net_short)});
            const std::optional<double> live_margin =
                row[10].is_null() ? std::nullopt : std::optional<double>(row[10].as<double>());
            if (live_margin.has_value() != cal->margin_bps.has_value() ||
                (live_margin && std::abs(*live_margin - *cal->margin_bps) > 1e-9)) {
                ++margin_differs;
            }
            // No validated margin -> nothing is actionable, live or replayed.
            const int replay_side = cal->margin_bps ? econ::decide_side(est, *cal->margin_bps / 10'000.0) : 0;
            int live_side = 0;
            if (live_margin) {
                if (live_net_long > *live_margin) live_side = 1;
                else if (live_net_short > *live_margin) live_side = -1;
            }
            if (live_side != 0) ++live_actionable;
            if (replay_side != live_side) ++decision_differs;
            const auto& arch = in[0][2];
            const bool arch_observed = !arch.is_null() && cal->cost_model.at(arch.as<double>()).spread_observed;
            if (arch_observed == observed &&
                (!observed || std::abs(arch.as<double>() - live_spread) <= 1e-4 * std::max(1.0, live_spread))) {
                ++spread_archive_match;
            } else {
                ++spread_archive_differs;
            }
        }
    });
    if (!q) { std::fprintf(stderr, "replay query failed: %s\n", q.error().message.c_str()); return 1; }
    std::printf("%s\n", json({{"model_id", model_id},
                              {"objective", objective},
                              {"policy_version", cal->policy_version},
                              {"live_predictions_compared", compared},
                              {"no_archived_input", no_input},
                              {"match_1e-6", exact},
                              {"match_1e-4", within_1e4},
                              {"max_abs_prob_diff", max_diff},
                              {"aligned_availability_differs", missing_differs},
                              {"nondeterministic_rescore", nondeterministic},
                              {"rows_with_cost_inputs", cost_rows},
                              {"cost_model_version_differs", cost_version_differs},
                              {"max_abs_net_diff_bps", max_net_diff_bps},
                              {"policy_margin_differs", margin_differs},
                              {"decision_differs", decision_differs},
                              {"live_actionable", live_actionable},
                              {"spread_input_matches_archive", spread_archive_match},
                              {"spread_input_differs_from_archive", spread_archive_differs},
                              {"magnitude_source", cal->magnitude_source},
                              {"rows_with_magnitudes", magnitude_rows},
                              {"max_abs_magnitude_diff_bps", max_mag_diff_bps}})
                            .dump().c_str());
    return 0;
}

int run_training_once(const config::RuntimeConfig& rt, bool force) {
    auto cfg = load_trainer_config(rt.root);
    if (!cfg) {
        std::fprintf(stderr, "config error: %s\n", cfg.error().message.c_str());
        return 2;
    }
    persistence::PgPool pool(rt.common.postgres_dsn, 1);
    if (!force) {
        if (const auto pressure = resource_pressure(*cfg)) {
            std::fprintf(stderr, "JOB_DEFERRED_RESOURCE_PRESSURE: %s\n", pressure->c_str());
            return 3;
        }
    }
    if (cfg->mode == "econ") {
        auto outcomes = run_econ_cycle(pool, rt, *cfg);
        if (!outcomes) {
            std::fprintf(stderr, "training failed: %s\n", outcomes.error().message.c_str());
            return 1;
        }
        for (const auto& o : *outcomes) {
            std::printf("{\"event\":\"training_done\",\"model_id\":\"%s\",\"status\":\"%s\","
                        "\"detail\":\"%s\"}\n",
                        o.model_id.c_str(), o.status.c_str(), o.detail.c_str());
        }
        return 0;
    }
    auto outcome = run_cycle(pool, rt, *cfg);
    if (!outcome) {
        std::fprintf(stderr, "training failed: %s\n", outcome.error().message.c_str());
        return 1;
    }
    std::printf("{\"event\":\"training_done\",\"model_id\":\"%s\",\"status\":\"%s\","
                "\"detail\":\"%s\"}\n",
                outcome->model_id.c_str(), outcome->status.c_str(), outcome->detail.c_str());
    return 0;
}

} // namespace altml
