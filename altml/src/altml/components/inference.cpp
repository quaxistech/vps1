#include "altml/components/inference.hpp"

#include <LightGBM/c_api.h>
#include <nlohmann/json.hpp>

#include "altml/components/econ_calibration.hpp"
#include "altml/components/econ_logic.hpp"
#include "altml/components/feature_recipe.hpp"
#include "altml/components/inference_logic.hpp"
#include "altml/components/feature_archive.hpp"
#include "altml/components/trainer_logic.hpp"
#include "altml/core/codec.hpp"
#include "altml/core/hash.hpp"
#include "altml/core/verified_artifact.hpp"
#include "altml/core/logger.hpp"
#include "altml/core/pg.hpp"
#include "altml/core/readiness.hpp"
#include "altml/core/shutdown.hpp"
#include "altml/core/sysinfo.hpp"
#include "altml/core/time.hpp"

// Alttech owns the analytics wire format; we vendor its proto (alttech.v1).
#include "alttech/system.pb.h"  // vendored alttech.v1

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <set>
#include <sstream>

namespace altml {

namespace {

using json = nlohmann::json;

// Bridge the vendored alttech.v1 feature message into the altml.v1 type the
// inference logic already understands. Every field number/enum value is byte
// identical between the two packages (verified against both feature.proto); the
// packages differ only in name, so this is a lossless re-pack, not a conversion.
using inference::repack_feature_envelope;


// AltTech's Timeframe enum is minutes; the archive stores the label. A vector
// on a grid this does not know is archived under no label at all -- it is
// dropped -- rather than filed under someone else's.
std::optional<std::string> timeframe_label(altml::v1::Timeframe tf) {
    switch (tf) {
        case altml::v1::TF_1M: return "1m";
        case altml::v1::TF_5M: return "5m";
        case altml::v1::TF_15M: return "15m";
        case altml::v1::TF_30M: return "30m";
        case altml::v1::TF_1H: return "1h";
        case altml::v1::TF_4H: return "4h";
        case altml::v1::TF_1D: return "1d";
        case altml::v1::TF_1S: return "1s";
        case altml::v1::TF_5S: return "5s";
        case altml::v1::TF_15S: return "15s";
        case altml::v1::TF_30S: return "30s";
        default: return std::nullopt;
    }
}

// One ACTIVE econ model (one per horizon), rebuilt ENTIRELY from its own
// registry row: recipe, sources, calibration, class means, cost. Never from the
// running config -- a config edited after training would rebuild the input
// differently and the booster would silently score a different space.
struct EconModel {
    std::string model_id;
    int model_version = 1;
    std::string objective;
    std::string recipe_version;      // DT-02/DT-04: the recipe it was trained on; served its projection
    int horizon_minutes = 0;
    std::string schema_hash;        // the target's talib-core layout
    int published_width = 0;        // width of the vector AltTech publishes
    int feature_count = 0;          // booster input width
    std::vector<size_t> base_positions;
    recipe::DerivedRecipe base_derived;
    struct Source {
        training::AlignedSource src;   // projection/positions = the columns taken
        size_t published_width = 0;
        bool derived = false;
        recipe::DerivedRecipe recipe;
    };
    std::vector<Source> sources;
    double temperature = 1.0;
    econ::ClassMeans means;
    // Cost model v2 (the decision cost at each T0) and the model's validated
    // policy margin. A model trained before policy-2 carries no margin: its
    // estimates are published and stored, and nothing it says is actionable.
    econ::CostModelV2 cost_model;
    std::optional<double> margin_bps;
    std::string policy_version;
    bool probability_calibrated = false;
    std::string calibration_version;
    std::string label_definition;
    std::string magnitude_source;
    // Served in SHADOW: the newest model of an objective that has no ACTIVE
    // one, rejected by the economic gates but still a better forecaster than
    // the base rates on its holdout. Its estimates are published and stored so
    // setups can be scored against a live model -- and marked SHADOW so that
    // no decision layer can act on them.
    bool shadow = false;
    BoosterHandle booster = nullptr;
    // Magnitude regressions (up / down excursion), present when the model's
    // magnitude_source is "regression".
    BoosterHandle up_booster = nullptr;
    BoosterHandle down_booster = nullptr;

    ~EconModel() {
        if (booster != nullptr) LGBM_BoosterFree(booster);
        if (up_booster != nullptr) LGBM_BoosterFree(up_booster);
        if (down_booster != nullptr) LGBM_BoosterFree(down_booster);
    }
};

// "name:version" -- the identity of a feature set, used to key its layout.
std::string source_set_key(const training::AlignedSource& src) {
    return src.feature_set + ":" + std::to_string(src.feature_set_version);
}

bool is_within(const std::filesystem::path& root, const std::filesystem::path& child) {
    auto root_it = root.begin();
    auto child_it = child.begin();
    for (; root_it != root.end(); ++root_it, ++child_it) {
        if (child_it == child.end() || *root_it != *child_it) return false;
    }
    return child_it != child.end();
}

class InferenceService {
    // Point-in-time observations from the source venues and grids the active
    // model reads, held only as the projected columns and only for as long as a
    // freshness bound could still admit them. The trainer got these from a
    // LATERAL over the archive; live, they arrive on the same subscription that
    // feeds the archive, so the served vector is built from what had actually
    // been received by T0 -- not from rows written after the fact.
    using AlignedObservation = inference::AlignedObservation;
    // venue, instrument, grid, feature set: one venue and grid publishes more
    // than one set, and they are different observations of different things.
    using AlignedKey = std::tuple<int, int64_t, std::string, std::string>;
    static constexpr size_t kAlignedHistory = 8;
    static size_t history_limit(std::string_view timeframe) {
        if (timeframe == "1s") return 256;
        if (timeframe == "5s") return 128;
        if (timeframe == "15s") return 32;
        if (timeframe == "30s") return 16;
        return kAlignedHistory;
    }

public:
    InferenceService(persistence::PgPool& pool, messaging::NatsClient& nats,
                     int64_t stale_after_sec, std::filesystem::path model_root,
                     std::string subject_prefix, bool serve_shadow,
                     const std::vector<std::string>& capture_sources)
        : pool_(pool), nats_(nats), stale_after_ms_(stale_after_sec * 1000),
          model_root_(std::move(model_root)), subject_prefix_(std::move(subject_prefix)),
          serve_shadow_(serve_shadow) {
        for (size_t i = 0; i < capture_sources.size(); ++i) {
            if (const auto source = training::parse_aligned_source(capture_sources[i], i)) {
                fast_capture_sources_.insert(std::tuple{source->exchange_id, source->timeframe, source->feature_set});
            }
        }
    }

    // Polls the registry for the ACTIVE econ models (one per objective) and
    // loads, replaces or drops them. A model whose input cannot be rebuilt
    // exactly -- recipe, feature names, width, calibration -- is not served:
    // scoring it with a differently shaped vector would produce numbers that
    // look like predictions and are not.
    void refresh_model() {
        struct Row {
            std::string model_id, objective, path, checksum, hash, recipe_version, names_sha,
                calibration;
            int version = 1, fcount = 0, base_count = 0;
            std::vector<std::string> source_specs, base_projection;
            bool shadow = false;
        };
        std::vector<Row> rows;
        std::vector<std::string> core_layout;
        std::map<std::string, std::pair<std::vector<std::string>, std::vector<int>>> set_layouts;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            for (const auto& r : txn.exec(
                     "SELECT m.model_id, m.model_version, m.objective, a.path, a.checksum, "
                     "       c.schema_hash, c.feature_count, c.base_feature_count, "
                     "       c.aligned_sources::text, c.base_projection::text, c.recipe_version, "
                     "       c.feature_names_sha256, c.calibration::text "
                     "FROM altml.models m "
                     "JOIN altml.model_artifacts a ON a.model_id = m.model_id AND a.kind = 'model' "
                     "JOIN altml.model_feature_compatibility c ON c.model_id = m.model_id "
                     "WHERE m.status = 'ACTIVE' AND m.algorithm = 'lightgbm' "
                     "ORDER BY m.objective")) {
                Row row;
                row.model_id = r[0].as<std::string>();
                row.version = r[1].as<int>();
                row.objective = r[2].as<std::string>();
                row.path = r[3].as<std::string>();
                row.checksum = r[4].as<std::string>();
                row.hash = r[5].as<std::string>();
                row.fcount = r[6].as<int>();
                row.base_count = r[7].as<int>();
                row.source_specs = training::parse_pg_text_array(r[8].as<std::string>());
                row.base_projection = training::parse_pg_text_array(r[9].as<std::string>());
                row.recipe_version = r[10].as<std::string>();
                row.names_sha = r[11].as<std::string>();
                row.calibration = r[12].as<std::string>();
                rows.push_back(std::move(row));
            }
            // SHADOW: for each econ objective WITHOUT an active model, the newest
            // econ model rejected by the gates whose holdout log-loss skill was
            // positive -- it forecasts better than the base rates even though
            // acting on it did not pay. Never for an objective that has an
            // ACTIVE model, and never a model whose probabilities were no better
            // than the base rates.
            if (serve_shadow_) {
                for (const auto& r : txn.exec(
                         "SELECT DISTINCT ON (m.objective) m.model_id, m.model_version, m.objective, "
                         "       a.path, a.checksum, c.schema_hash, c.feature_count, "
                         "       c.base_feature_count, c.aligned_sources::text, "
                         "       c.base_projection::text, c.recipe_version, "
                         "       c.feature_names_sha256, c.calibration::text "
                         "FROM altml.models m "
                         "JOIN altml.model_artifacts a ON a.model_id = m.model_id AND a.kind = 'model' "
                         "JOIN altml.model_feature_compatibility c ON c.model_id = m.model_id "
                         "JOIN altml.model_metrics k ON k.model_id = m.model_id "
                         "     AND k.scope = 'test' AND k.metric = 'logloss_skill' AND k.value > 0 "
                         "WHERE m.status = 'REJECTED' AND m.algorithm = 'lightgbm' "
                         "  AND m.objective LIKE 'multiclass:econ_class_%' "
                         "  AND NOT EXISTS (SELECT 1 FROM altml.models x WHERE x.status = 'ACTIVE' "
                         "                  AND x.objective = m.objective) "
                         "ORDER BY m.objective, m.created_at DESC")) {
                    Row row;
                    row.model_id = r[0].as<std::string>();
                    row.version = r[1].as<int>();
                    row.objective = r[2].as<std::string>();
                    row.path = r[3].as<std::string>();
                    row.checksum = r[4].as<std::string>();
                    row.hash = r[5].as<std::string>();
                    row.fcount = r[6].as<int>();
                    row.base_count = r[7].as<int>();
                    row.source_specs = training::parse_pg_text_array(r[8].as<std::string>());
                    row.base_projection = training::parse_pg_text_array(r[9].as<std::string>());
                    row.recipe_version = r[10].as<std::string>();
                    row.names_sha = r[11].as<std::string>();
                    row.calibration = r[12].as<std::string>();
                    row.shadow = true;
                    rows.push_back(std::move(row));
                }
            }
            // Where each frozen bitget_micro version keeps the book spread:
            // the decision cost of every estimate reads it (cost model v2).
            std::map<int, size_t> pos;
            for (const auto& r : txn.exec(
                     "SELECT v.version, m.position FROM altml.fdw_feature_set_versions v "
                     "JOIN altml.fdw_feature_set_members m ON m.feature_set_version_id = v.id "
                     "WHERE v.name = 'bitget_micro' AND v.frozen AND m.output_name = 'book_spread_bps'")) {
                pos[r[0].as<int>()] = r[1].as<size_t>();
            }
            {
                std::lock_guard lock(mu_);
                spread_pos_ = std::move(pos);
            }
            if (rows.empty()) return;
            // The target's own layout, resolved by the models' schema hash.
            for (const auto& r : txn.exec_params(
                     "SELECT fm.output_name FROM altml.fdw_feature_set_members fm "
                     "JOIN altml.fdw_feature_set_versions fv ON fv.id = fm.feature_set_version_id "
                     "WHERE fv.schema_hash = $1 ORDER BY fm.position",
                     rows.front().hash)) {
                core_layout.push_back(r[0].as<std::string>());
            }
            for (const auto& row : rows) {
                for (size_t i = 0; i < row.source_specs.size(); ++i) {
                    const auto src = training::parse_aligned_source(row.source_specs[i], i);
                    if (!src || src->feature_set == altml::analytics::kCoreFeatureSetName) continue;
                    const std::string key = source_set_key(*src);
                    if (set_layouts.contains(key)) continue;
                    auto& lay = set_layouts[key];
                    for (const auto& r : txn.exec_params(
                             "SELECT fm.output_name FROM altml.fdw_feature_set_members fm "
                             "JOIN altml.fdw_feature_set_versions fv ON fv.id = fm.feature_set_version_id "
                             "WHERE fv.name = $1 AND fv.version = $2 AND fv.frozen ORDER BY fm.position",
                             src->feature_set, src->feature_set_version)) {
                        lay.first.push_back(r[0].as<std::string>());
                    }
                    std::string joined;
                    for (const auto& c : lay.first) { joined += c; joined += '|'; }
                    for (const auto& r : txn.exec_params(
                             "SELECT v.version, string_agg(m.output_name, '|' ORDER BY m.position) || '|' "
                             "FROM altml.fdw_feature_set_versions v "
                             "JOIN altml.fdw_feature_set_members m ON m.feature_set_version_id = v.id "
                             "WHERE v.name = $1 AND v.frozen GROUP BY v.version",
                             src->feature_set)) {
                        if (r[1].as<std::string>() == joined) lay.second.push_back(r[0].as<int>());
                    }
                }
            }
        });
        if (!res) {
            log::warn("model_poll_failed", {{"message", res.error().message}});
            return;
        }

        std::map<std::string, std::unique_ptr<EconModel>> next;
        {
            std::lock_guard lock(mu_);
            for (auto& [objective, model] : models_) {
                const auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) {
                    return r.objective == objective && r.model_id == model->model_id &&
                           r.shadow == model->shadow;
                });
                if (it != rows.end()) next[objective] = std::move(model);   // unchanged
                else log::warn("model_deactivated", {{"model_id", model->model_id},
                                                     {"objective", objective}});
            }
        }
        std::vector<training::AlignedSource> fresh_sources;
        for (const auto& row : rows) {
            if (next.contains(row.objective)) continue;
            if (auto m = load_econ_model(row.model_id, row.version, row.objective, row.path,
                                         row.checksum, row.hash, row.fcount, row.base_count,
                                         row.source_specs, row.base_projection, row.recipe_version,
                                         row.names_sha, row.calibration, core_layout, set_layouts)) {
                m->shadow = row.shadow;
                log::info("model_activated_in_process",
                          {{"model_id", m->model_id}, {"objective", m->objective},
                           {"role", m->shadow ? "SHADOW" : "ACTIVE"},
                           {"horizon_minutes", m->horizon_minutes},
                           {"features", m->feature_count},
                           {"aligned_sources", static_cast<int64_t>(m->sources.size())},
                           {"temperature", m->temperature}});
                for (const auto& s : m->sources) fresh_sources.push_back(s.src);
                next[row.objective] = std::move(m);
            }
        }
        // Cost evidence has the same restart obligation as model features.
        // Prime every frozen spread layout even if no model selected micro
        // columns, then apply its own receipt cutoff at each target.
        {
            std::lock_guard lock(mu_);
            if (!spread_pos_.empty()) {
                training::AlignedSource spread_source;
                spread_source.feature_set = "bitget_micro";
                spread_source.freshness_minutes = 5;
                spread_source.feature_set_version = spread_pos_.begin()->first;
                for (const auto& [version, _] : spread_pos_) spread_source.equivalent_versions.push_back(version);
                fresh_sources.push_back(std::move(spread_source));
            }
        }
        auto primed = load_aligned_from_archive(fresh_sources);
        std::lock_guard lock(mu_);
        for (auto& [key, obs] : primed) {
            if (std::get<0>(key) == 1 && std::get<2>(key) == "1m" && std::get<3>(key) == "bitget_micro") {
                const auto pos = spread_pos_.find(obs.feature_set_version);
                if (pos != spread_pos_.end() && pos->second < obs.values.size() &&
                    std::isfinite(obs.values[pos->second])) {
                    auto spread_obs = obs;
                    spread_obs.values = {obs.values[pos->second]};
                    inference::record_aligned_revision(spread_[std::get<1>(key)], std::move(spread_obs), kAlignedHistory);
                }
            }
            inference::record_aligned_revision(aligned_cache_[key], std::move(obs), history_limit(std::get<2>(key)));
        }
        models_ = std::move(next);
    }

    // A restart empties the aligned cache, and it refills only as each grid's
    // next bar closes -- up to an hour for the 1h source. Every prediction in
    // between was scored with its aligned block all NaN, an input the model
    // almost never met in training (the point-in-time join finds the previous
    // bar), and published as an ordinary estimate: aligned_missing 162 of 162
    // after the 2026-09-25 09:37 restart. The archive holds what the live
    // subscription delivered, every row received before now, so it is exactly
    // what a service that had never stopped would hold; the cache is primed
    // from it before a newly loaded model scores anything. latest_aligned()
    // still applies the freshness bound, so nothing older than the live rule
    // admits is ever used. A failed read leaves the cache to refill live.
    std::vector<std::pair<AlignedKey, AlignedObservation>> load_aligned_from_archive(
        const std::vector<training::AlignedSource>& sources) {
        std::vector<std::pair<AlignedKey, AlignedObservation>> out;
        if (sources.empty()) return out;
        std::set<std::string> seen;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            for (const auto& src : sources) {
                if (!seen.insert(std::to_string(src.exchange_id) + ":" + src.timeframe + ":" +
                                 source_set_key(src) + ":" + src.engine_version + ":" +
                                 std::to_string(src.pinned_instrument_id.value_or(-1)))
                         .second) {
                    continue;
                }
                std::string versions = "{";
                for (const int v : src.equivalent_versions) {
                    if (versions.size() > 1) versions += ',';
                    versions += std::to_string(v);
                }
                versions += "}";
                const int64_t window_min = (training::source_span_ms(src) *
                    static_cast<int64_t>(history_limit(src.timeframe) + 1) + 59'999) / 60'000 +
                    src.freshness_minutes;
                std::string sql =
                    "SELECT instrument_id, (extract(epoch FROM bar_open_time) * 1000)::bigint, "
                    "       feature_set_version, values::text, validity_bitmap::text, bar_revision, "
                    "       (extract(epoch FROM computed_at) * 1000)::bigint, "
                    "       (extract(epoch FROM received_at) * 1000)::bigint, engine_version "
                    "FROM altml.econ_feature_vectors "
                    "WHERE exchange_id = $1 AND timeframe = $2 AND feature_set_name = $3 "
                    "AND feature_set_version = ANY($4::int[]) AND quality_status = 'VALID' "
                    "AND bar_open_time >= now() - make_interval(mins => $5::int) ";
                if (src.pinned_instrument_id.has_value()) {
                    sql += "AND instrument_id = " + std::to_string(*src.pinned_instrument_id) + " ";
                }
                sql += "ORDER BY instrument_id, bar_open_time, bar_revision";
                for (const auto& r : txn.exec_params(sql, src.exchange_id, src.timeframe,
                                                     src.feature_set, versions, window_min)) {
                    AlignedObservation obs;
                    obs.bar_open_ms = r[1].as<int64_t>();
                    obs.feature_set_version = r[2].as<int32_t>();
                    obs.bar_revision = r[5].as<int64_t>();
                    obs.computed_at_ms = r[6].as<int64_t>();
                    obs.received_at_ms = r[7].as<int64_t>();
                    obs.engine_version = r[8].as<std::string>();
                    obs.values = training::parse_pg_float_array(r[3].as<std::string>());
                    const auto bmp = training::parse_pg_hex_bytea(r[4].as<std::string>());
                    for (size_t i = 0; i < obs.values.size(); ++i) {
                        const bool valid = (i / 8) < bmp.size() && ((bmp[i / 8] >> (i % 8)) & 1U) != 0U;
                        if (!valid) obs.values[i] = std::numeric_limits<float>::quiet_NaN();
                    }
                    out.emplace_back(AlignedKey{src.exchange_id, r[0].as<int64_t>(), src.timeframe,
                                                src.feature_set},
                                     std::move(obs));
                }
            }
        });
        if (!res) {
            log::warn("aligned_prime_failed", {{"message", res.error().message}});
            out.clear();
        } else if (!out.empty()) {
            log::info("aligned_primed_from_archive",
                      {{"observations", static_cast<int64_t>(out.size())},
                       {"sources", static_cast<int64_t>(seen.size())}});
        }
        return out;
    }

    std::unique_ptr<EconModel> load_econ_model(
        const std::string& model_id, int version, const std::string& objective,
        const std::string& path, const std::string& checksum, const std::string& hash, int fcount,
        int base_count, const std::vector<std::string>& source_specs,
        const std::vector<std::string>& base_projection, const std::string& recipe_version,
        const std::string& names_sha, const std::string& calibration_text,
        const std::vector<std::string>& core_layout,
        const std::map<std::string, std::pair<std::vector<std::string>, std::vector<int>>>& set_layouts) {
        const auto refuse = [&](const char* why) -> std::unique_ptr<EconModel> {
            log::error("model_refused", {{"model_id", model_id}, {"objective", objective},
                                         {"reason", why}});
            return nullptr;
        };
        // Only the econ objective family is served, and only with a recipe
        // this binary knows. A legacy direction model has no economic
        // estimate to offer; serving it would reintroduce the probability
        // AltStrategy no longer has a use for.
        if (!objective.starts_with(econ::kObjectivePrefix)) return refuse("objective_not_served");
        const auto horizon = econ::objective_horizon_minutes(objective);
        if (!horizon) return refuse("unknown_horizon");
        if (recipe::projection_for(recipe_version).empty()) return refuse("recipe_version_unknown");

        std::error_code root_error, artifact_error;
        const auto canonical_root = std::filesystem::weakly_canonical(model_root_, root_error);
        const auto canonical_artifact = std::filesystem::canonical(path, artifact_error);
        if (root_error || artifact_error || !is_within(canonical_root, canonical_artifact)) {
            return refuse("model_path_outside_root");
        }
        const std::string safe_path = canonical_artifact.string();
        auto model_bytes = verified_model_bytes(safe_path, checksum);
        if (!model_bytes) return refuse("checksum_mismatch");

        auto m = std::make_unique<EconModel>();
        m->model_id = model_id;
        m->model_version = version;
        m->objective = objective;
        m->recipe_version = recipe_version;
        m->horizon_minutes = *horizon;
        m->schema_hash = hash;
        m->published_width = static_cast<int>(core_layout.size());
        m->feature_count = fcount;

        // Calibration: temperature, class means, cost, validated margin -- one
        // parser, shared with `altml replay`, that refuses a malformed record
        // instead of throwing it out of the model poller.
        econ::CalibrationError cal_error;
        const auto cal = econ::parse_calibration(json::parse(calibration_text, nullptr, false), &cal_error);
        if (!cal) return refuse(cal_error.reason.c_str());
        if (cal->horizon_minutes != *horizon) return refuse("calibration_inconsistent");
        m->temperature = cal->temperature;
        m->means = cal->means;
        m->cost_model = cal->cost_model;
        m->margin_bps = cal->margin_bps;
        m->policy_version = cal->policy_version;
        m->probability_calibrated = cal->probability_calibrated;
        m->calibration_version = cal->calibration_version;
        m->label_definition = cal->label_definition;
        m->magnitude_source = cal->magnitude_source;

        // Base: the projection, by name, in the target's layout.
        std::vector<std::string> names;
        for (const auto& n : base_projection) {
            const auto it = std::find(core_layout.begin(), core_layout.end(), n);
            if (it == core_layout.end()) return refuse("base_projection_unresolved");
            m->base_positions.push_back(static_cast<size_t>(it - core_layout.begin()));
        }
        recipe::append_base_names(base_projection, names);
        m->base_derived = recipe::DerivedRecipe::resolve(core_layout);
        if (!m->base_derived.ok) return refuse("derived_unresolved");
        if (static_cast<int>(base_projection.size() + recipe::DerivedRecipe::width()) != base_count) {
            return refuse("base_width_mismatch");
        }
        for (size_t i = 0; i < source_specs.size(); ++i) {
            const auto parsed = training::parse_aligned_source(source_specs[i], i);
            if (!parsed) return refuse("aligned_source_unusable");
            EconModel::Source s;
            s.src = *parsed;
            if (!recipe::source_method_compatible(recipe_version, s.src.feature_set, s.src.engine_version))
                return refuse("source_method_unpinned_or_incompatible");
            if (s.src.feature_set == altml::analytics::kCoreFeatureSetName) {
                s.published_width = core_layout.size();
                s.src.equivalent_versions = {analytics::kCoreFeatureSetVersion};
                for (const auto& n : recipe::projection_for(recipe_version)) {
                    const auto it = std::find(core_layout.begin(), core_layout.end(), n);
                    if (it == core_layout.end()) return refuse("source_projection_unresolved");
                    s.src.projection.push_back(n);
                    s.src.positions.push_back(static_cast<size_t>(it - core_layout.begin()));
                }
                s.derived = true;
                s.recipe = recipe::DerivedRecipe::resolve(core_layout);
                if (!s.recipe.ok) return refuse("source_derived_unresolved");
            } else {
                const auto it = set_layouts.find(source_set_key(s.src));
                if (it == set_layouts.end() || it->second.first.empty() || it->second.second.empty()) {
                    return refuse("source_layout_unresolved");
                }
                s.src.equivalent_versions = it->second.second;
                const auto& layout = it->second.first;
                if (!recipe::source_layout_compatible(s.src.feature_set, s.src.feature_set_version, layout))
                    return refuse("source_layout_incompatible");
                s.published_width = layout.size();
                for (size_t k = 0; k < layout.size(); ++k) {
                    if (recipe::is_excluded(s.src.feature_set, layout[k])) continue;
                    s.src.projection.push_back(layout[k]);
                    s.src.positions.push_back(k);
                }
            }
            recipe::append_source_names(
                recipe::source_prefix(s.src.exchange_id, s.src.timeframe, s.src.feature_set),
                s.src.projection, s.derived, names);
            m->sources.push_back(std::move(s));
        }
        if (static_cast<int>(names.size()) != fcount) return refuse("rebuilt_width_mismatch");
        if (sha256_hex(recipe::joined_names(names)) != names_sha) return refuse("feature_names_mismatch");

        int iterations = 0;
        if (LGBM_BoosterLoadModelFromString(model_bytes->c_str(), &iterations, &m->booster) != 0) {
            return refuse("model_load_failed");
        }
        int nf = 0, nc = 0;
        LGBM_BoosterGetNumFeature(m->booster, &nf);
        LGBM_BoosterGetNumClasses(m->booster, &nc);
        if (nf != fcount || nc != 3) return refuse("booster_shape_mismatch");
        // Magnitude models: the same trust rules as the classifier (inside the
        // model root, checksum as registered, the registered width). A model
        // that declares regression magnitudes and cannot serve them is refused
        // rather than silently served with a different magnitude source.
        if (m->magnitude_source == "regression") {
            std::map<std::string, std::pair<std::string, std::string>> arts;
            auto rr = pool_.with_txn([&](pqxx::work& txn) {
                for (const auto& r : txn.exec_params(
                         "SELECT kind, path, checksum FROM altml.model_artifacts "
                         "WHERE model_id = $1 AND kind IN ('model_up', 'model_down')", model_id)) {
                    arts[r[0].as<std::string>()] = {r[1].as<std::string>(), r[2].as<std::string>()};
                }
            });
            if (!rr || arts.size() != 2) return refuse("magnitude_artifacts_missing");
            for (auto [kind, handle] : {std::pair{"model_up", &m->up_booster}, std::pair{"model_down", &m->down_booster}}) {
                const auto& [apath, asum] = arts.at(kind);
                std::error_code ae;
                const auto canon = std::filesystem::canonical(apath, ae);
                if (ae || !is_within(canonical_root, canon)) return refuse("magnitude_path_outside_root");
                auto magnitude_bytes = verified_model_bytes(canon.string(), asum);
                if (!magnitude_bytes) return refuse("magnitude_checksum_mismatch");
                int it = 0;
                if (LGBM_BoosterLoadModelFromString(magnitude_bytes->c_str(), &it, handle) != 0) {
                    return refuse("magnitude_model_load_failed");
                }
                int mf = 0, mc = 0;
                LGBM_BoosterGetNumFeature(*handle, &mf);
                LGBM_BoosterGetNumClasses(*handle, &mc);
                if (mf != fcount || mc != 1) return refuse("magnitude_shape_mismatch");
            }
        }
        return m;
    }

    // AltTech itself only retains a few days of `venue_feature_vectors` (sized
    // for live order attribution, not model training). AltML archives every
    // VALID vector it consumes here, building its own sufficiently long,
    // real-observed training substrate (see altml.feature_vectors retention).
    // The original live vector must commit before inference or input ACK.
    Result<void> archive_feature_vector(const altml::v1::Envelope& env,
                                const altml::v1::FeatureVectorV1& fv,
                                const std::string& tf_label, std::string_view original_payload,
                                int64_t* durable_received_at_ms) {
        // exchange_id and timeframe were the literals `1` and `'1m'` here. That
        // was harmless only while the subscription could deliver nothing else:
        // the moment a second venue or grid arrives, a literal files Binance's
        // 5m vector under Bitget 1m and the corruption is invisible, because
        // every column still looks plausible. They are parameters now.
        const auto schema = resolved_schema_hash(fv.feature_set_name(), fv.feature_set_version(), fv.schema_hash());
        if (!schema) return std::unexpected(schema.error());
        auto res = inference::archive_live_feature(pool_, env, fv, tf_label, *schema, original_payload,
                                                   durable_received_at_ms);
        if (!res) {
            archive_failed_.fetch_add(1, std::memory_order_relaxed);
            log::warn("feature_vector_archive_failed", {{"message", res.error().message}});
            return std::unexpected(res.error());
        }
        return {};
    }

    // Pull upstream vectors AltTech still holds but this archive has not seen.
    //
    // The live subscription only ever delivers what is flowing right now, and
    // the AltTech stream is byte-capped -- in practice it carries hours, not
    // days. Anything older than that exists in exactly one place before it is
    // dropped: AltTech's own table, which retains days. This walks forward from
    // what the archive already holds, one bounded chunk per venue/grid per
    // cycle, so a restart or an outage of any length is repaired without ever
    // rescanning history that is already stored.
    //
    // ON CONFLICT DO NOTHING makes it idempotent against the live writer: both
    // paths may reach the same bar, and neither overwrites the other.
    int64_t backfill_archive_from_upstream(int venue, const std::string& tf, int limit) {
        int64_t inserted = 0;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto r = txn.exec_params(
                "INSERT INTO altml.feature_vectors (exchange_id, instrument_id, timeframe, "
                "bar_open_time, bar_revision, feature_set_name, feature_set_version, "
                "schema_hash, engine_version, values, validity_bitmap, quality_status, "
                "computed_at, ingest_path) "
                "SELECT v.exchange_id, v.canonical_pair_id, v.timeframe, v.bar_open_time, "
                "       v.bar_revision, fs.name, fs.version, fs.schema_hash, v.engine_version, "
                "       v.values, v.validity_bitmap, 'VALID', v.computed_at, 'backfill' "
                "FROM altml.fdw_venue_feature_vectors v "
                "JOIN altml.fdw_feature_set_versions fs ON fs.id = v.feature_set_version_id "
                "WHERE v.exchange_id = $1 AND v.timeframe = $2 AND v.status = 'VALID' "
                // Bounded by retention: rows older than the archive keeps would
                // be inserted and dropped again on the next cleanup pass, and
                // the scan that finds them is the expensive part -- measured 36
                // seconds per venue/grid over the unbounded table.
                "  AND v.bar_open_time > now() - interval '45 days' "
                // The two writers must not race for the same bar. Live owns the
                // present; this owns history. Without the horizon the backfill
                // wins whenever the NATS consumer is even slightly behind --
                // measured here, it won every recent bar while the durable
                // worked through a 78k backlog -- and the archive fills with
                // recompute-regime values while production inference consumes
                // live-regime ones. That is the parity the model depends on, so
                // it is settled by construction rather than by who is faster.
                "  AND v.bar_open_time < now() - interval '15 minutes' "
                // Fill what is MISSING, not what is newer than a watermark. A
                // high-water mark is wrong the moment the live path writes a
                // recent bar first: max() jumps forward and every hole behind it
                // becomes permanent, because nothing ever looks back again.
                "  AND NOT EXISTS (SELECT 1 FROM altml.feature_vectors a "
                "                  WHERE a.exchange_id = v.exchange_id "
                "                    AND a.instrument_id = v.canonical_pair_id "
                "                    AND a.timeframe = v.timeframe "
                "                    AND a.bar_open_time = v.bar_open_time "
                "                    AND a.bar_revision = v.bar_revision) "
                "ORDER BY v.bar_open_time "
                "LIMIT $3 "
                "ON CONFLICT (exchange_id, instrument_id, timeframe, bar_open_time, "
                "bar_revision, schema_hash) DO NOTHING",
                venue, tf, limit);
            inserted = r.affected_rows();
        });
        if (!res) {
            archive_failed_.fetch_add(1, std::memory_order_relaxed);
            log::warn("feature_archive_backfill_failed",
                      {{"exchange_id", venue}, {"timeframe", tf},
                       {"message", res.error().message}});
            return 0;
        }
        return inserted;
    }

    // The archive's primary key includes schema_hash, and the two writers did not
    // agree on it: AltTech leaves the field EMPTY on the wire for reference_lite
    // and cross_venue_core, so live ingestion stored "" while the FDW backfill
    // stored the registry's value for the very same bar. Two rows, one bar, two
    // keys -- the archive would have carried both and nothing would have said so.
    //
    // Resolve against the frozen owner's dictionary even when the wire carries
    // a hash. Otherwise a wrong-hash vector could enter an aligned source by
    // matching only its name/version, despite its claimed incompatible schema.
    Result<std::string> resolved_schema_hash(const std::string& set_name, int set_version,
                                     const std::string& wire_hash) {
        const auto verify = [&](const std::string& registered) -> Result<std::string> {
            if (!wire_hash.empty() && wire_hash != registered)
                return fail(ErrorCode::validation_error, "source_schema_hash_mismatch");
            return registered;
        };
        const std::string key = set_name + ":" + std::to_string(set_version);
        {
            std::lock_guard lock(hash_mu_);
            const auto it = schema_hash_cache_.find(key);
            if (it != schema_hash_cache_.end()) return verify(it->second);
        }
        std::string resolved;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec_params(
                "SELECT schema_hash FROM altml.fdw_feature_set_versions "
                "WHERE name = $1 AND version = $2 AND frozen",
                set_name, set_version);
            if (!rows.empty()) resolved = rows[0][0].as<std::string>();
        });
        if (!res) return std::unexpected(res.error());
        if (resolved.empty()) return fail(ErrorCode::not_found, "source_schema_unavailable", true);
        std::lock_guard lock(hash_mu_);
        schema_hash_cache_[key] = resolved;
        return verify(resolved);
    }

    // Cross-venue consensus history. The live path gets these from NATS, but the
    // stream is capped by bytes and holds hours; AltTech's own table holds days,
    // and a model trained on a 30-day window needs the days.
    //
    // Only STRICT_VALID is archived, which is exactly what the live path admits:
    // DEGRADED_n_OF_4 decodes to INCOMPLETE_INPUT and is not archived there
    // either. Backfilling it here would put rows in the archive that live
    // ingestion would never have written -- the two paths must agree about what
    // counts as an observation, or the training substrate stops matching what
    // production actually sees.
    int64_t backfill_cross_venue_from_upstream(int limit) {
        int64_t inserted = 0;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto r = txn.exec_params(
                "INSERT INTO altml.feature_vectors (exchange_id, instrument_id, timeframe, "
                "bar_open_time, bar_revision, feature_set_name, feature_set_version, "
                "schema_hash, engine_version, values, validity_bitmap, quality_status, "
                "computed_at, ingest_path) "
                // exchange_id 0: a consensus belongs to no venue. instrument_id
                // is the canonical pair, which is the same key every venue row
                // uses (AltTech guarantees canonical_pair_id == instrument_id
                // for usdt_perp). bar_revision 0: a consensus is not revised.
                "SELECT 0, v.canonical_pair_id, v.timeframe, v.bar_open_time, 0, "
                "       fs.name, fs.version, fs.schema_hash, v.method_version, "
                "       v.values, v.validity_bitmap, 'VALID', v.computed_at, 'backfill' "
                "FROM altml.fdw_cross_venue_feature_vectors v "
                "JOIN altml.fdw_feature_set_versions fs ON fs.id = v.feature_set_version_id "
                "WHERE v.timeframe = '1m' AND v.status = 'VALID' "
                "  AND v.quality_status = 'STRICT_VALID' "
                "  AND v.bar_open_time > now() - interval '45 days' "
                "  AND v.bar_open_time < now() - interval '15 minutes' "
                "  AND NOT EXISTS (SELECT 1 FROM altml.feature_vectors a "
                "                  WHERE a.exchange_id = 0 "
                "                    AND a.instrument_id = v.canonical_pair_id "
                "                    AND a.timeframe = v.timeframe "
                "                    AND a.bar_open_time = v.bar_open_time "
                "                    AND a.bar_revision = 0) "
                "ORDER BY v.bar_open_time LIMIT $1 "
                "ON CONFLICT (exchange_id, instrument_id, timeframe, bar_open_time, "
                "bar_revision, schema_hash) DO NOTHING",
                limit);
            inserted = r.affected_rows();
        });
        if (!res) {
            archive_failed_.fetch_add(1, std::memory_order_relaxed);
            log::warn("cross_venue_backfill_failed", {{"message", res.error().message}});
            return 0;
        }
        return inserted;
    }

    // The macro / cross-asset block.
    //
    // Archived under exchange_id 0 AND instrument_id 0: it belongs to no venue
    // and no pair. USDCNH does not depend on which crypto instrument is being
    // predicted, so it is stored once and the aligned source pins the join to
    // that key. Writing one copy per instrument would multiply four hundred
    // thousand rows into eleven million carrying the same numbers.
    //
    // Published on no NATS subject, so this wrapper is the only path to it.
    int64_t backfill_macro_from_upstream(int limit) {
        int64_t inserted = 0;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto r = txn.exec_params(
                "INSERT INTO altml.feature_vectors (exchange_id, instrument_id, timeframe, "
                "bar_open_time, bar_revision, feature_set_name, feature_set_version, "
                "schema_hash, engine_version, values, validity_bitmap, quality_status, "
                "computed_at, ingest_path) "
                "SELECT 0, 0, v.timeframe, v.bar_open_time, 0, "
                "       fs.name, fs.version, fs.schema_hash, v.method_version, "
                "       v.values, v.validity_bitmap, 'VALID', v.computed_at, 'backfill' "
                "FROM altml.fdw_macro_feature_vectors v "
                "JOIN altml.fdw_feature_set_versions fs ON fs.id = v.feature_set_version_id "
                "WHERE v.timeframe = '1m' "
                // WARMING vectors are the lead-in of a chunk, before the
                // lookbacks and publication lags have caught up. They carry no
                // usable position and would occupy archive rows saying nothing.
                "  AND v.status <> 'WARMING' "
                // The macro block exists to sit beside the crypto features in a
                // training row. A macro row with no crypto row at the same
                // minute joins to nothing and teaches nothing, so the backfill
                // starts where the archived crypto features start -- not where
                // the partitions start, and not where AltTech's ten months of
                // macro history start. The target venue's first archived bar is
                // the cheapest honest answer to "where does crypto begin here".
                "  AND v.bar_open_time >= (SELECT bar_open_time FROM altml.feature_vectors "
                "                          WHERE exchange_id = 1 AND instrument_id = "
                "                                (SELECT min(instrument_id) FROM altml.feature_vectors "
                "                                 WHERE exchange_id = 1 AND feature_set_name = 'talib-core') "
                "                            AND feature_set_name = 'talib-core' "
                "                          ORDER BY bar_open_time LIMIT 1) "
                "  AND v.bar_open_time < now() - interval '15 minutes' "
                "  AND NOT EXISTS (SELECT 1 FROM altml.feature_vectors a "
                "                  WHERE a.exchange_id = 0 AND a.instrument_id = 0 "
                "                    AND a.timeframe = v.timeframe "
                "                    AND a.bar_open_time = v.bar_open_time "
                "                    AND a.schema_hash = fs.schema_hash) "
                "ORDER BY v.bar_open_time LIMIT $1 "
                "ON CONFLICT DO NOTHING",
                limit);
            inserted = r.affected_rows();
        });
        if (!res) {
            archive_failed_.fetch_add(1, std::memory_order_relaxed);
            log::warn("macro_backfill_failed", {{"message", res.error().message}});
            return 0;
        }
        return inserted;
    }

    // Microstructure history, 1m window only -- the archive is keyed by a bar
    // grid and 15s/30s windows have no bar to belong to.
    //
    // This wrapper is the ONLY path to reference_lite_micro: AltTech computes it
    // for the three information venues and publishes it on no NATS subject at
    // all, so nothing that listens to the stream can ever see it.
    int64_t backfill_microstructure_from_upstream(int venue, int limit) {
        int64_t inserted = 0;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto r = txn.exec_params(
                "INSERT INTO altml.feature_vectors (exchange_id, instrument_id, timeframe, "
                "bar_open_time, bar_revision, feature_set_name, feature_set_version, "
                "schema_hash, engine_version, values, validity_bitmap, quality_status, "
                "computed_at, ingest_path) "
                // A tumbling window opens where its bar opens, and is not revised.
                "SELECT v.exchange_id, v.canonical_pair_id, '1m', v.window_start, 0, "
                "       fs.name, fs.version, fs.schema_hash, v.engine_version, "
                "       v.values, v.validity_bitmap, 'VALID', v.computed_at, 'backfill' "
                "FROM altml.fdw_venue_microstructure_vectors v "
                "JOIN altml.fdw_feature_set_versions fs ON fs.id = v.feature_set_version_id "
                "WHERE v.window_label = '1m' AND v.status = 'VALID' AND v.exchange_id = $1 "
                "  AND v.window_start > now() - interval '45 days' "
                "  AND v.window_start < now() - interval '15 minutes' "
                "  AND NOT EXISTS (SELECT 1 FROM altml.feature_vectors a "
                "                  WHERE a.exchange_id = v.exchange_id "
                "                    AND a.instrument_id = v.canonical_pair_id "
                "                    AND a.timeframe = '1m' "
                "                    AND a.bar_open_time = v.window_start "
                "                    AND a.bar_revision = 0 "
                "                    AND a.schema_hash = fs.schema_hash) "
                "ORDER BY v.window_start LIMIT $2 "
                "ON CONFLICT (exchange_id, instrument_id, timeframe, bar_open_time, "
                "bar_revision, schema_hash) DO NOTHING",
                venue, limit);
            inserted = r.affected_rows();
        });
        if (!res) {
            archive_failed_.fetch_add(1, std::memory_order_relaxed);
            log::warn("microstructure_backfill_failed",
                      {{"exchange_id", venue}, {"message", res.error().message}});
            return 0;
        }
        return inserted;
    }

    // One bounded pass over every venue and grid AltTech computes.
    void backfill_archive_cycle(int chunk) {
        static constexpr int kVenues[] = {1, 2, 3, 4};
        static const char* kGrids[] = {"1m", "5m", "15m", "30m", "1h", "4h", "1d"};
        int64_t total = 0;
        // Sources that arrived late go FIRST and in bigger chunks. The venue/grid
        // sweep costs ~36 seconds per combination even when it inserts nothing,
        // and there are 28 of them; behind that queue a newly added source would
        // fill at a few thousand rows per 20-minute pass, which for ~800k rows of
        // history is a day of catching up before a model can see any of it.
        static constexpr int kNewSourceMultiplier = 10;
        const int new_chunk = chunk * kNewSourceMultiplier;
        total += backfill_cross_venue_from_upstream(new_chunk);
        total += backfill_macro_from_upstream(new_chunk);
        for (const int venue : kVenues) {
            total += backfill_microstructure_from_upstream(venue, new_chunk);
        }
        for (const int venue : kVenues) {
            for (const char* tf : kGrids) {
                total += backfill_archive_from_upstream(venue, tf, chunk);
            }
        }
        if (total > 0) {
            log::info("feature_archive_backfilled", {{"rows", total}});
        }
    }

    // A successful false means an intentional skip; errors must be NAKed so a
    // transient inference/publication failure cannot silently lose a result.
    // Record a source observation if any active model reads that (venue, grid,
    // set, version). The FULL vector is kept, invalid slots as NaN: different
    // models take different columns from it, and talib-core sources also feed
    // the derived block, which needs inputs the projection does not keep.
    void remember_aligned(const altml::v1::Envelope& env, const altml::v1::FeatureVectorV1& fv,
                          const std::string& tf_label, int64_t received_at_ms) {
        std::lock_guard lock(mu_);
        if (models_.empty()) return;
        // Deliberately NOT filtered on engine_version: each set is produced by
        // its own engine, and the trainer's LATERAL matches on set name and
        // version only.
        const int venue = static_cast<int>(env.exchange());
        bool wanted = false;
        for (const auto& [_, m] : models_) {
            for (const auto& s : m->sources) {
                const auto& ev = s.src.equivalent_versions;
                if (s.src.exchange_id == venue && s.src.timeframe == tf_label &&
                    s.src.feature_set == fv.feature_set_name() &&
                    std::find(ev.begin(), ev.end(), fv.feature_set_version()) != ev.end()) {
                    wanted = true;
                    break;
                }
            }
            if (wanted) break;
        }
        if (!wanted) return;
        AlignedObservation obs;
        obs.bar_open_ms = fv.bar_open_time_ms();
        obs.feature_set_version = fv.feature_set_version();
        obs.bar_revision = fv.bar_revision();
        obs.computed_at_ms = fv.computed_at_ms();
        obs.received_at_ms = received_at_ms;
        obs.engine_version = fv.engine_version();
        obs.values.resize(static_cast<size_t>(fv.values_size()));
        const std::string& bmp = fv.validity_bitmap();
        for (int i = 0; i < fv.values_size(); ++i) {
            const auto u = static_cast<size_t>(i);
            const bool valid = (u / 8) < bmp.size() &&
                               (((static_cast<uint8_t>(bmp[u / 8]) >> (u % 8)) & 1U) != 0U);
            obs.values[u] = valid ? fv.values(i) : std::numeric_limits<float>::quiet_NaN();
        }
        auto& hist = aligned_cache_[AlignedKey{venue, env.instrument_id(), tf_label, fv.feature_set_name()}];
        inference::record_aligned_revision(hist, std::move(obs), history_limit(tf_label));
    }

    // The execution venue's observed book spread, kept for the decision cost.
    void remember_spread(const altml::v1::Envelope& env, const altml::v1::FeatureVectorV1& fv,
                         const std::string& tf_label, int64_t received_at_ms) {
        if (env.exchange() != altml::v1::EXCHANGE_BITGET || tf_label != "1m" ||
            fv.feature_set_name() != "bitget_micro") {
            return;
        }
        std::lock_guard lock(mu_);
        const auto it = spread_pos_.find(fv.feature_set_version());
        if (it == spread_pos_.end()) return;
        const size_t p = it->second;
        const std::string& bmp = fv.validity_bitmap();
        const bool valid = p < static_cast<size_t>(fv.values_size()) && (p / 8) < bmp.size() &&
                           (((static_cast<uint8_t>(bmp[p / 8]) >> (p % 8)) & 1U) != 0U);
        if (!valid) return;
        if (!std::isfinite(fv.values(static_cast<int>(p)))) return;
        inference::record_aligned_revision(spread_[env.instrument_id()],
            AlignedObservation{fv.bar_open_time_ms(), fv.feature_set_version(),
                {fv.values(static_cast<int>(p))}, fv.bar_revision(), fv.computed_at_ms(), received_at_ms},
            kAlignedHistory);
    }

    // The spread the book showed at T0 (the latest window at or before the
    // target bar, at most five minutes old), or NaN: the cost model then uses
    // its stated fallback and the estimate says so. Caller holds mu_.
    double spread_at(int64_t instrument_id, int64_t bar_open_ms, int64_t received_at_ms) const {
        const auto it = spread_.find(instrument_id);
        if (it == spread_.end()) return std::nan("");
        for (auto obs = it->second.rbegin(); obs != it->second.rend(); ++obs) {
            const int64_t age = bar_open_ms - obs->bar_open_ms;
            if (age < 0 || age > 5 * 60'000 || obs->received_at_ms <= 0 ||
                obs->received_at_ms > received_at_ms || obs->values.empty()) continue;
            return obs->values.front();
        }
        return std::nan("");
    }

    // The newest observation of `src` a live system has at T0. Caller holds mu_.
    const AlignedObservation* latest_aligned(const EconModel::Source& source,
        int64_t instrument_id, int64_t t0_ms, int64_t computed_at_ms, int64_t received_at_ms) const {
        const auto& src = source.src;
        const int64_t keyed = src.pinned_instrument_id.value_or(instrument_id);
        const auto it = aligned_cache_.find(AlignedKey{src.exchange_id, keyed, src.timeframe, src.feature_set});
        if (it == aligned_cache_.end()) return nullptr;
        for (auto obs = it->second.rbegin(); obs != it->second.rend(); ++obs) {
            const auto& ev = src.equivalent_versions;
            if (std::find(ev.begin(), ev.end(), obs->feature_set_version) == ev.end()) continue;
            if (obs->values.size() != source.published_width) continue;
            if (!src.engine_version.empty() && obs->engine_version != src.engine_version) continue;
            if (training::aligned_observation_usable_pit(src, obs->bar_open_ms,
                    obs->computed_at_ms, obs->received_at_ms, t0_ms, computed_at_ms, received_at_ms)) return &*obs;
        }
        return nullptr;
    }

    Result<bool> on_feature_vector(const altml::v1::Envelope& env, std::string_view original_payload) {
        const auto& fv = env.feature_vector();
        const int64_t now = timeutil::now_ms();
        int64_t received_at_ms = 0;

        // ARCHIVE PATH -- every venue, every grid, every feature set, recorded
        // with its own provenance. What a model may READ is decided below.
        const auto tf = timeframe_label(fv.timeframe());
        // Fast data is archived only for explicitly configured training sources
        // or loaded recipes. The producer owns continuous raw history; this
        // bounded consumer must not turn an unselected1s stream into45days of
        // extra rows for every venue/instrument.
        if (tf && tf->ends_with('s')) {
            std::lock_guard lock(mu_);
            bool selected = fast_capture_sources_.contains(
                std::tuple{static_cast<int>(env.exchange()), *tf, fv.feature_set_name()});
            for (const auto& [_, model] : models_) {
                for (const auto& source : model->sources) {
                    selected = selected || (source.src.exchange_id == static_cast<int>(env.exchange()) &&
                        source.src.timeframe == *tf && source.src.feature_set == fv.feature_set_name());
                }
            }
            if (!selected) return false;
        }
        // Counted, not just skipped: a vector that decodes but is not VALID
        // leaves no other trace.
        if (tf && fv.status() != altml::v1::FEATURE_STATUS_VALID) {
            not_valid_skipped_.fetch_add(1, std::memory_order_relaxed);
        }
        if (tf && fv.status() == altml::v1::FEATURE_STATUS_VALID) {
            auto archived = archive_feature_vector(env, fv, *tf, original_payload, &received_at_ms);
            if (!archived) return std::unexpected(archived.error());
            remember_aligned(env, fv, *tf, received_at_ms);
            remember_spread(env, fv, *tf, received_at_ms);
        }

        // MODEL-INPUT PATH -- the target is a Bitget instrument on the 1m grid.
        if (env.exchange() != altml::v1::EXCHANGE_BITGET) return false;
        if (fv.timeframe() != altml::v1::TF_1M) return false;

        std::lock_guard lock(mu_);
        if (models_.empty()) return false;   // no model: no estimate, never a default one
        const int64_t t0_ms = fv.bar_open_time_ms() + 60'000;
        bool any = false;
        for (const auto& [objective, mp] : models_) {
            const EconModel& m = *mp;
            const std::string rejected = inference::feature_rejection_reason(
                fv, now, stale_after_ms_, m.schema_hash, m.published_width);
            if (!rejected.empty()) {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                if (rejected == "stale_or_future_bar" || rejected == "stale_or_future_computation") {
                    stale_dropped_.fetch_add(1, std::memory_order_relaxed);
                }
                continue;
            }
            // The target's full vector, invalid slots NaN, then the model's
            // own recipe over it and over each aligned source.
            std::vector<float> full(static_cast<size_t>(fv.values_size()));
            const std::string& bmp = fv.validity_bitmap();
            for (int i = 0; i < fv.values_size(); ++i) {
                const auto u = static_cast<size_t>(i);
                const bool valid = (u / 8) < bmp.size() &&
                                   (((static_cast<uint8_t>(bmp[u / 8]) >> (u % 8)) & 1U) != 0U);
                full[u] = valid ? fv.values(i) : std::numeric_limits<float>::quiet_NaN();
            }
            std::vector<double> x;
            x.reserve(static_cast<size_t>(m.feature_count));
            for (const size_t p : m.base_positions) x.push_back(static_cast<double>(full[p]));
            std::vector<float> derived(recipe::DerivedRecipe::width());
            m.base_derived.compute(full.data(), full.size(), derived.data());
            for (const float v : derived) x.push_back(static_cast<double>(v));
            int missing = 0;
            for (const auto& s : m.sources) {
                const auto* obs = latest_aligned(s, env.instrument_id(), t0_ms,
                                                 fv.computed_at_ms(), received_at_ms);
                const size_t width = s.src.positions.size() + (s.derived ? derived.size() : 0);
                if (obs == nullptr) {
                    ++missing;
                    x.insert(x.end(), width, std::nan(""));
                    continue;
                }
                for (const size_t p : s.src.positions) {
                    x.push_back(p < obs->values.size() ? static_cast<double>(obs->values[p]) : std::nan(""));
                }
                if (s.derived) {
                    s.recipe.compute(obs->values.data(), obs->values.size(), derived.data());
                    for (const float v : derived) x.push_back(static_cast<double>(v));
                }
            }
            aligned_slots_.fetch_add(static_cast<int64_t>(m.sources.size()), std::memory_order_relaxed);
            aligned_missing_.fetch_add(missing, std::memory_order_relaxed);
            if (static_cast<int>(x.size()) != m.feature_count) {
                return fail(ErrorCode::internal_error, "econ input width mismatch for " + m.model_id);
            }
            double raw[3] = {0, 0, 0};
            int64_t out_len = 0;
            const auto t_start = std::chrono::steady_clock::now();
            const int rc = LGBM_BoosterPredictForMatSingleRow(
                m.booster, x.data(), C_API_DTYPE_FLOAT64, static_cast<int32_t>(x.size()), 1,
                C_API_PREDICT_RAW_SCORE, 0, -1, "", &out_len, raw);
            const auto t_end = std::chrono::steady_clock::now();
            if (rc != 0 || out_len != 3) {
                return fail(ErrorCode::internal_error,
                            std::string("LightGBM prediction failed: ") + LGBM_GetLastError(), true);
            }
            const auto p = econ::softmax3(raw, m.temperature);
            const auto cost = m.cost_model.at(spread_at(env.instrument_id(), fv.bar_open_time_ms(), received_at_ms));
            const auto est = econ::estimate(p, m.means, cost.fraction());
            // Expected excursions (bps, >= 0): the regressions when the model
            // serves them, else the class means.
            double up_bps = std::max(est.expected_mfe_long * 10'000.0, 0.0);
            double down_bps = std::max(-est.expected_mae_long * 10'000.0, 0.0);
            if (m.up_booster != nullptr && m.down_booster != nullptr) {
                double yu = 0, yd = 0;
                int64_t lu = 0, ld = 0;
                const int ru = LGBM_BoosterPredictForMatSingleRow(
                    m.up_booster, x.data(), C_API_DTYPE_FLOAT64, static_cast<int32_t>(x.size()), 1,
                    C_API_PREDICT_NORMAL, 0, -1, "", &lu, &yu);
                const int rd = LGBM_BoosterPredictForMatSingleRow(
                    m.down_booster, x.data(), C_API_DTYPE_FLOAT64, static_cast<int32_t>(x.size()), 1,
                    C_API_PREDICT_NORMAL, 0, -1, "", &ld, &yd);
                if (ru != 0 || rd != 0 || lu != 1 || ld != 1 || !std::isfinite(yu) || !std::isfinite(yd)) {
                    return fail(ErrorCode::internal_error,
                                std::string("LightGBM magnitude prediction failed: ") + LGBM_GetLastError(), true);
                }
                up_bps = std::max(std::expm1(yu), 0.0);
                down_bps = std::max(std::expm1(yd), 0.0);
            }
            const int64_t latency_us =
                std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();

            messaging::EnvelopeMeta meta;
            meta.event_type = "prediction";
            meta.instrument_id = env.instrument_id();
            meta.exchange_symbol = env.exchange_symbol();
            meta.source_service = "altml";
            meta.message_id = "pred:" + env.exchange_symbol() + ":1m:" +
                              std::to_string(fv.bar_open_time_ms()) + ":r" +
                              std::to_string(fv.bar_revision()) + ":" + m.model_id;
            auto out = messaging::make_envelope(meta);
            auto* pr = out.mutable_prediction();
            pr->set_prediction_id(meta.message_id);
            pr->set_timeframe(altml::v1::TF_1M);
            pr->set_bar_open_time_ms(fv.bar_open_time_ms());
            pr->set_bar_revision(fv.bar_revision());
            pr->set_model_id(m.model_id);
            pr->set_model_version(m.model_version);
            pr->set_feature_set_version(std::string(analytics::kCoreFeatureSetName) + ":" +
                                        std::to_string(analytics::kCoreFeatureSetVersion));
            // Schema 2: the class probabilities of the econ target. "up" is
            // LONG-favourable (a long clears the all-in cost over the
            // horizon), "down" SHORT-favourable; the remainder is NEUTRAL.
            pr->set_probability_up(est.p_long);
            pr->set_probability_down(est.p_short);
            pr->set_probability_long(est.p_long);
            pr->set_probability_neutral(est.p_neutral);
            pr->set_probability_short(est.p_short);
            pr->set_expected_return(est.expected_return);
            pr->set_expected_mfe(est.expected_mfe_long);
            pr->set_expected_mae(est.expected_mae_long);
            // The target is a fixed-horizon hold: the holding time IS the horizon.
            pr->set_expected_holding_time_min(static_cast<double>(m.horizon_minutes));
            pr->set_expected_net_return_long_bps(est.expected_net_long * 10'000.0);
            pr->set_expected_net_return_short_bps(est.expected_net_short * 10'000.0);
            pr->set_round_trip_cost_bps(cost.bps());
            // Schema 3: the cost by component, the policy margin, magnitudes.
            pr->set_cost_model_version(econ::kCostModelVersion);
            pr->set_fee_bps(cost.fee_rt_bps);
            pr->set_spread_bps(cost.spread_bps);
            pr->set_impact_bps(cost.impact_rt_bps);
            pr->set_spread_observed(cost.spread_observed);
            if (m.margin_bps) pr->set_policy_margin_bps(*m.margin_bps);
            pr->set_policy_version(m.policy_version);
            // A short's favourable excursion is the long's adverse one.
            pr->set_favorable_long_bps(est.expected_mfe_long * 10'000.0);
            pr->set_adverse_long_bps(est.expected_mae_long * 10'000.0);
            pr->set_favorable_short_bps(-est.expected_mae_long * 10'000.0);
            pr->set_adverse_short_bps(-est.expected_mfe_long * 10'000.0);
            // Schema 4: market-prediction semantics. The excursions as
            // magnitudes (>= 0); the calibration verdict of the holdout test;
            // what up / flat / down mean; the model's role.
            pr->set_probability_calibrated(m.probability_calibrated);
            pr->set_calibration_version(m.calibration_version);
            pr->set_label_definition(m.label_definition);
            pr->set_expected_up_excursion_bps(up_bps);
            pr->set_expected_down_excursion_bps(down_bps);
            pr->set_magnitude_source(m.magnitude_source);
            pr->set_model_role(m.shadow ? "SHADOW" : "ACTIVE");
            pr->set_horizon_minutes(m.horizon_minutes);
            pr->set_objective(m.objective);
            pr->set_calibrator(econ::kCalibratorVersion);
            pr->set_recipe_version(m.recipe_version);
            pr->set_aligned_missing(missing);
            pr->set_confidence(std::abs(est.p_long - est.p_short));
            // SHADOW outranks everything: a shadow estimate is never actionable,
            // whatever its inputs looked like.
            pr->set_quality_status(m.shadow ? "SHADOW" : (missing == 0 ? "VALID" : "DEGRADED"));
            pr->set_computed_at_ms(timeutil::now_ms());
            pr->set_latency_us(latency_us);
            pr->set_prediction_schema_version(analytics::kPredictionSchemaVersion);
            pr->set_target_venue(static_cast<altml::v1::Exchange>(analytics::kTargetExchangeId));
            pr->set_abstain(false);

            const std::string subject = subject_prefix_ + "." + env.exchange_symbol() + ".1m.h" +
                                        std::to_string(m.horizon_minutes);
            if (auto r = nats_.js_publish(subject, messaging::serialize(out), out.message_id(), 5000); !r) {
                return std::unexpected(r.error());
            }
            persist_prediction(env.instrument_id(), fv, m, est, missing, cost, up_bps, down_bps);
            latency_acc_.fetch_add(latency_us, std::memory_order_relaxed);
            any = true;
        }
        return any;
    }

    // Every live estimate, with what it was an estimate OF. Best effort: a
    // failed write is counted and reported, never allowed to hold back the
    // live stream -- but it is visible in inference_stats.
    void persist_prediction(int64_t instrument_id, const altml::v1::FeatureVectorV1& fv,
                            const EconModel& m, const econ::Estimate& e, int missing,
                            const econ::CostAt& cost, double up_bps, double down_bps) {
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec_params(
                "INSERT INTO altml.model_predictions (exchange_id, instrument_id, bar_open_time, "
                "bar_revision, model_id, objective, horizon_minutes, p_short, p_neutral, p_long, "
                "expected_return, expected_net_long_bps, expected_net_short_bps, "
                "round_trip_cost_bps, calibrator, recipe_version, aligned_missing, serving_role, "
                "cost_model_version, spread_bps, spread_observed, policy_margin_bps, "
                "expected_mfe_long_bps, expected_mae_long_bps, expected_up_excursion_bps, "
                "expected_down_excursion_bps, magnitude_source, probability_calibrated) "
                "VALUES (1, $1, to_timestamp($2::bigint / 1000.0), $3, $4, $5, $6, $7, $8, $9, "
                "$10, $11, $12, $13, $14, $15, $16, $17, $18, $19, $20, $21, $22, $23, $24, $25, "
                "$26, $27) "
                "ON CONFLICT DO NOTHING",
                instrument_id, fv.bar_open_time_ms(), fv.bar_revision(), m.model_id, m.objective,
                m.horizon_minutes, e.p_short, e.p_neutral, e.p_long, e.expected_return,
                e.expected_net_long * 10'000.0, e.expected_net_short * 10'000.0, cost.bps(),
                std::string(econ::kCalibratorVersion), m.recipe_version, missing,
                std::string(m.shadow ? "SHADOW" : "ACTIVE"), std::string(econ::kCostModelVersion),
                cost.spread_bps, cost.spread_observed, m.margin_bps,
                e.expected_mfe_long * 10'000.0, e.expected_mae_long * 10'000.0, up_bps, down_bps,
                m.magnitude_source, m.probability_calibrated);
        });
        if (!res) persist_failed_.fetch_add(1, std::memory_order_relaxed);
    }

    void log_stats(int64_t predictions) {
        const int64_t lat = latency_acc_.exchange(0);
        // Aligned coverage is reported, not smoothed over: the trainer saw
        // these sources in an archive that had settled, while live they can be
        // late or absent. A model scored with most of its aligned block missing
        // is being served outside the conditions it learned in, and that has to
        // be visible rather than inferred from a drop in quality later.
        const int64_t slots = aligned_slots_.exchange(0);
        const int64_t missing = aligned_missing_.exchange(0);
        log::info("inference_stats",
                  {{"predictions", predictions},
                   {"stale_dropped", stale_dropped_.exchange(0)},
                   {"rejected", rejected_.exchange(0)},
                   {"avg_latency_us", predictions > 0 ? lat / predictions : 0},
                   {"aligned_slots", slots},
                   {"aligned_missing", missing},
                   {"not_valid_skipped", not_valid_skipped_.exchange(0)},
                   {"persist_failed", persist_failed_.exchange(0)},
                   {"active_models", active_models()},
                   {"mode", has_model() ? "DETERMINISTIC_PLUS_ML" : "DETERMINISTIC_ONLY"}});
    }

    bool has_model() {
        std::lock_guard lock(mu_);
        return !models_.empty();
    }

    int64_t active_models() {
        std::lock_guard lock(mu_);
        return static_cast<int64_t>(models_.size());
    }

private:
    persistence::PgPool& pool_;
    messaging::NatsClient& nats_;
    int64_t stale_after_ms_;
    std::filesystem::path model_root_;
    std::string subject_prefix_;
    bool serve_shadow_ = true;
    std::mutex mu_;
    // One ACTIVE econ model per objective (per horizon).
    std::map<std::string, std::unique_ptr<EconModel>> models_;

    std::map<AlignedKey, std::deque<AlignedObservation>> aligned_cache_;
    std::set<std::tuple<int, std::string, std::string>> fast_capture_sources_;
    // The latest Bitget book spread per instrument (bitget_micro, 1m window):
    // bar open time and value in bps. Guarded by mu_.
    std::map<int64_t, std::deque<AlignedObservation>> spread_;
    std::map<int, size_t> spread_pos_;   // bitget_micro version -> position

    std::atomic<int64_t> stale_dropped_{0};
    std::atomic<int64_t> rejected_{0};
    std::atomic<int64_t> latency_acc_{0};
    std::atomic<int64_t> archive_failed_{0};
    std::mutex hash_mu_;
    std::map<std::string, std::string> schema_hash_cache_;
    std::atomic<int64_t> not_valid_skipped_{0};
    std::atomic<int64_t> aligned_slots_{0};
    std::atomic<int64_t> aligned_missing_{0};
    std::atomic<int64_t> persist_failed_{0};
};

} // namespace

struct InferenceComponent::Impl {
    std::optional<persistence::PgPool> pool;
    std::optional<InferenceService> service;
    std::optional<messaging::PullConsumer> consumer;
    // inference.min_backfill_free_disk_gb: below this much free space the
    // archive backfill thread stands down (see run()). 10 GB is ~5% of VPS1's
    // filesystem, the EMERGENCY line of the host's storage policy.
    int64_t min_backfill_free_disk_gb = 10;
};

InferenceComponent::InferenceComponent(const config::RuntimeConfig& rt,
                                       messaging::NatsClient& nats)
    : rt_(rt), nats_(nats), impl_(std::make_unique<Impl>()) {}

InferenceComponent::~InferenceComponent() = default;

Result<void> InferenceComponent::init() {
    const YAML::Node section = rt_.root["inference"];
    // `stale_prediction_sec` is the public configuration key.  Keep the old
    // spelling as a compatibility fallback for installations that may have
    // deployed it before the example configuration was standardised.
    const int64_t stale_after_sec = config::opt_int(
        section, "stale_prediction_sec", config::opt_int(section, "stale_after_sec", 90));
    impl_->min_backfill_free_disk_gb = config::opt_int(section, "min_backfill_free_disk_gb",
                                                       impl_->min_backfill_free_disk_gb);
    if (impl_->min_backfill_free_disk_gb < 1 || impl_->min_backfill_free_disk_gb > 1000) {
        return fail(ErrorCode::config_error, "inference.min_backfill_free_disk_gb out of range [1, 1000]");
    }
    if (stale_after_sec < 10 || stale_after_sec > 600) {
        return fail(ErrorCode::config_error,
                    "stale_prediction_sec out of range [10, 600]: " +
                        std::to_string(stale_after_sec));
    }
    const std::string model_root = rt_.project.model_root;

    impl_->pool.emplace(rt_.common.postgres_dsn,
                        static_cast<size_t>(config::opt_int(section, "postgres_pool_size", 2)));
    if (auto probe = impl_->pool->with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); });
        !probe) {
        return fail(ErrorCode::database_error,
                    "postgres unavailable: " + probe.error().message);
    }

    // inference.serve_shadow_models: publish the newest gate-rejected econ
    // model of an objective with no ACTIVE one, marked SHADOW (see EconModel).
    const bool serve_shadow = config::opt_bool(section, "serve_shadow_models", true);
    impl_->service.emplace(*impl_->pool, nats_, stale_after_sec, model_root,
                           rt_.project.prediction_subject_prefix, serve_shadow,
                           config::opt_str_list(rt_.root["training"], "econ_sources"));
    impl_->service->refresh_model();

    auto consumer = nats_.pull_consumer(rt_.project.input_stream, rt_.project.durable);
    if (!consumer) {
        return fail(ErrorCode::messaging_error,
                    "consumer bind failed: " + consumer.error().message);
    }
    impl_->consumer.emplace(std::move(*consumer));

    set_component_status(name(), impl_->service->has_model() ? "DETERMINISTIC_PLUS_ML"
                                                             : "DETERMINISTIC_ONLY (no model)");
    return {};
}

void InferenceComponent::run() {
    auto& shutdown = ShutdownToken::instance();
    auto& service = *impl_->service;
    auto& consumer = *impl_->consumer;
    int64_t last_stats = timeutil::now_ms();
    int64_t last_poll = 0;
    int64_t predictions = 0;

    // History repair runs on its OWN thread, never on the live one.
    //
    // It shared the fetch loop at first, and the cost was not theoretical: the
    // missing-row predicate makes each pass a foreign-table scan with a local
    // anti-join, and while it ran the durable fell to ~66 messages a minute
    // against a stream producing thousands. The live path then loses every race
    // for a recent bar and the archive fills with recompute-regime values --
    // the exact inversion of the parity production inference depends on.
    // And it is the one thing here that grows the disk on its own initiative:
    // a pass can add hundreds of MB of archive rows. Below the EMERGENCY line
    // of the host's storage policy (~5% of the filesystem) it stands down and
    // says so once, so that live ingestion and the trades keep the space.
    std::thread backfill_thread([&service, min_free_gb = impl_->min_backfill_free_disk_gb] {
        auto& stop = ShutdownToken::instance();
        bool standing_down = false;
        while (!stop.wait_for(std::chrono::seconds(60))) {
            const int64_t free_gb = sysinfo::free_disk_gb("/");
            if (free_gb >= 0 && free_gb < min_free_gb) {
                if (!standing_down) {
                    log::error("archive_backfill_paused_low_disk",
                               {{"free_disk_gb", free_gb}, {"min_free_disk_gb", min_free_gb}});
                    standing_down = true;
                }
                continue;
            }
            if (standing_down) {
                log::info("archive_backfill_resumed", {{"free_disk_gb", free_gb}});
                standing_down = false;
            }
            service.backfill_archive_cycle(2000);
        }
    });

    while (!shutdown.stop_requested()) {
        const int64_t now = timeutil::now_ms();
        if (now - last_poll >= 30'000) {
            service.refresh_model();
            last_poll = now;
        }
        auto msgs = consumer.fetch(64, 1000);
        if (!msgs) {
            if (shutdown.wait_for(std::chrono::seconds(1))) break;
            continue;
        }
        for (auto& msg : *msgs) {
            const auto raw = msg.data();
            alttech::v1::Envelope alt_env;
            altml::v1::Envelope env;
            // No payload pre-filter here: the decoder decides which shapes it
            // accepts, and it now accepts both. Testing has_feature_vector()
            // first is what dropped every venue vector before the decoder ever
            // saw one.
            const bool ok = alt_env.ParseFromArray(raw.data(), static_cast<int>(raw.size())) &&
                            repack_feature_envelope(alt_env, &env);
            if (!ok) {
                log::warn("inference_message_rejected", {{"message", "unsupported payload"}});
                if (auto terminated = msg.term(); !terminated) {
                    log::warn("inference_term_failed", {{"message", terminated.error().message}});
                }
                continue;
            }
            auto handled = service.on_feature_vector(env, raw);
            if (!handled) {
                if (handled.error().code == ErrorCode::validation_error && !handled.error().retryable) {
                    log::warn("inference_source_refused", {{"message", handled.error().message}});
                    if (auto terminated = msg.term(); !terminated)
                        log::warn("inference_term_failed", {{"message", terminated.error().message}});
                    continue;
                }
                log::warn("inference_message_retry",
                          {{"symbol", env.exchange_symbol()},
                           {"message", handled.error().message}});
                if (auto retried = msg.nak(1000); !retried) {
                    log::warn("inference_nak_failed", {{"message", retried.error().message}});
                }
                continue;
            }
            if (*handled) ++predictions;
            if (auto acknowledged = msg.ack(); !acknowledged) {
                log::warn("inference_ack_failed", {{"message", acknowledged.error().message}});
            }
        }
        if (now - last_stats >= 60'000) {
            service.log_stats(predictions);
            predictions = 0;
            last_stats = now;
        }
    }
    if (backfill_thread.joinable()) backfill_thread.join();
}

} // namespace altml
