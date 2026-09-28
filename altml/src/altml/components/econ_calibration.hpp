#pragma once

// The calibration record of an econ model, read ONE way.
//
// Inference and `altml replay` both turn the registry's calibration JSON into
// the numbers a decision is made from: temperature, class means, cost model,
// validated margin. Replay once kept its own reader, written for the policy-1
// record; when policy-2 replaced `round_trip_cost_bps` with a `cost_model`
// object, that reader indexed a missing key of a const json (undefined
// behaviour) and died on every policy-2 model -- the one check meant to prove
// live decisions reproducible could not even load them. A replay that parses
// the calibration differently from inference is not a replay, so there is one
// parser, and it validates every type instead of trusting the record: a
// malformed row is refused with a reason, never thrown out of the model poller.

#include "altml/components/econ_logic.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace altml::econ {

struct Calibration {
    double temperature = 1.0;
    int horizon_minutes = 0;
    ClassMeans means;
    CostModelV2 cost_model;
    // Present only where the margin PAID on validation: policy-2 as first
    // shipped kept the least-bad margin even when every one lost.
    std::optional<double> margin_bps;
    std::string policy_version;
    // Market-prediction semantics (prediction schema 4). A record from before
    // the calibration test carries none: its probabilities are published as
    // uncalibrated scores.
    bool probability_calibrated = false;
    std::string calibration_version;
    std::string label_definition;
    std::string magnitude_source = "class_means";
};

struct CalibrationError {
    std::string reason;   // stable code: calibration_incomplete, calibration_inconsistent, ...
};

[[nodiscard]] inline std::optional<Calibration> parse_calibration(const nlohmann::json& cal,
                                                                  CalibrationError* err = nullptr) {
    const auto fail = [&](const char* why) -> std::optional<Calibration> {
        if (err != nullptr) err->reason = why;
        return std::nullopt;
    };
    const auto num = [](const nlohmann::json& j, const char* k) {
        return j.contains(k) && j[k].is_number();
    };
    const auto triple = [](const nlohmann::json& j, const char* k) {
        if (!j.contains(k) || !j[k].is_array() || j[k].size() != 3) return false;
        for (const auto& v : j[k]) {
            if (!v.is_number()) return false;
        }
        return true;
    };
    if (!cal.is_object() || !num(cal, "temperature") || !triple(cal, "class_means_return") ||
        !triple(cal, "class_means_mfe_long") || !triple(cal, "class_means_mae_long") ||
        !(num(cal, "round_trip_cost_bps") || (cal.contains("cost_model") && cal["cost_model"].is_object())) ||
        !cal.contains("horizon_minutes") || !cal["horizon_minutes"].is_number_integer() ||
        !cal.contains("calibrator") || !cal["calibrator"].is_string() ||
        cal["calibrator"].get<std::string>() != kCalibratorVersion) {
        return fail("calibration_incomplete");
    }
    Calibration c;
    c.temperature = cal["temperature"].get<double>();
    c.horizon_minutes = cal["horizon_minutes"].get<int>();
    if (!(c.temperature > 0) || !std::isfinite(c.temperature) || c.horizon_minutes <= 0) {
        return fail("calibration_inconsistent");
    }
    for (size_t k = 0; k < 3; ++k) {
        c.means.ret[k] = cal["class_means_return"][k].get<double>();
        c.means.mfe[k] = cal["class_means_mfe_long"][k].get<double>();
        c.means.mae[k] = cal["class_means_mae_long"][k].get<double>();
        c.means.n[k] = 1;
    }
    if (cal.contains("cost_model") && cal["cost_model"].is_object()) {
        const auto& cm = cal["cost_model"];
        if (!cm.contains("version") || !cm["version"].is_string() ||
            cm["version"].get<std::string>() != kCostModelVersion) {
            return fail("cost_model_unknown");
        }
        const auto take = [&](const char* k, double& into) {
            if (num(cm, k)) into = cm[k].get<double>();
        };
        take("fee_bps_per_side", c.cost_model.fee_bps_per_side);
        take("impact_bps_per_side", c.cost_model.impact_bps_per_side);
        take("fallback_spread_bps", c.cost_model.fallback_spread_bps);
        take("max_plausible_spread_bps", c.cost_model.max_plausible_spread_bps);
    }
    // else: a model fitted on the flat v1 cost. Its DECISION cost is still v2
    // at each T0 -- the cost of acting is a fact about the book now, not about
    // how the model was trained -- with the default components.
    if (cal.contains("policy") && cal["policy"].is_object()) {
        const auto& pol = cal["policy"];
        c.policy_version = pol.contains("version") && pol["version"].is_string()
                               ? pol["version"].get<std::string>()
                               : std::string();
        const bool paid = num(pol, "validation_net_bps") && pol["validation_net_bps"].get<double>() > 0.0;
        if (paid && num(pol, "margin_bps")) c.margin_bps = pol["margin_bps"].get<double>();
    } else {
        c.policy_version = "policy-1";
    }
    c.calibration_version = cal["calibrator"].get<std::string>();
    if (cal.contains("calibration_version") && cal["calibration_version"].is_string())
        c.calibration_version = cal["calibration_version"].get<std::string>();
    c.label_definition = cal.contains("label_definition") && cal["label_definition"].is_string()
                             ? cal["label_definition"].get<std::string>()
                             : std::string("econ-class:cost-v1 (flat round-trip cost)");
    if (cal.contains("magnitude_source") && cal["magnitude_source"].is_string())
        c.magnitude_source = cal["magnitude_source"].get<std::string>();
    // Calibrated only on a PASSED holdout test; anything else (absent,
    // malformed, failed) publishes the numbers as uncalibrated scores.
    if (cal.contains("calibration_test") && cal["calibration_test"].is_object()) {
        const auto& t = cal["calibration_test"];
        c.probability_calibrated = t.contains("passed") && t["passed"].is_boolean() && t["passed"].get<bool>();
    }
    return c;
}

} // namespace altml::econ
