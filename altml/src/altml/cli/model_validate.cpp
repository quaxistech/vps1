// altml model-validate: native validation of a LightGBM model artifact.
//
//   altml model-validate --model <model.txt> --sample <sample_predictions.json>
//                        [--expected-features N] [--tolerance 1e-6]
//
// Checks:
//   1. the artifact loads through the LightGBM C API;
//   2. feature count matches the expected feature-set size;
//   3. C++ predictions match the Python predictions recorded at training time
//      (row-by-row, within tolerance) — the promotion gate for equivalence;
//   4. inference latency is reported (single-row, single-thread).
//
// Exit codes: 0 ok, 1 validation failed, 2 usage/load error.

#include "altml/cli/commands.hpp"

#include <LightGBM/c_api.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace altml::cli {

int cmd_model_validate(int argc, char** argv) {
    std::string model_path;
    std::string sample_path;
    int expected_features = -1;
    double tolerance = 1e-6;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--model" && i + 1 < argc) model_path = argv[++i];
        else if (a == "--sample" && i + 1 < argc) sample_path = argv[++i];
        else if ((a == "--expected-features" || a == "--tolerance") && i + 1 < argc) {
            // A malformed numeric argument must be a usage error, not a crash.
            try {
                if (a == "--expected-features") expected_features = std::stoi(argv[++i]);
                else tolerance = std::stod(argv[++i]);
            } catch (const std::exception&) {
                std::cerr << "ERROR: invalid numeric value for " << a << ": " << argv[i] << "\n";
                return 2;
            }
        }
        else if (a == "--help" || a == "-h") {
            std::cout << "usage: altml model-validate --model model.txt --sample sample.json\n"
                         "           [--expected-features N] [--tolerance 1e-6]\n";
            return 0;
        }
    }
    if (model_path.empty() || sample_path.empty()) {
        std::cerr << "ERROR: --model and --sample are required\n";
        return 2;
    }

    BoosterHandle booster = nullptr;
    int iterations = 0;
    if (LGBM_BoosterCreateFromModelfile(model_path.c_str(), &iterations, &booster) != 0) {
        std::cerr << "ERROR: cannot load model: " << LGBM_GetLastError() << "\n";
        return 2;
    }
    int n_features = 0;
    LGBM_BoosterGetNumFeature(booster, &n_features);
    std::cout << "model loaded: iterations=" << iterations << " features=" << n_features << "\n";
    if (expected_features > 0 && n_features != expected_features) {
        std::cerr << "ERROR: feature count mismatch: model=" << n_features
                  << " expected=" << expected_features << "\n";
        return 1;
    }

    std::ifstream in(sample_path);
    if (!in.good()) {
        std::cerr << "ERROR: cannot read sample file\n";
        return 2;
    }
    nlohmann::json sample = nlohmann::json::parse(in, nullptr, false);
    if (sample.is_discarded() || !sample.contains("rows") || !sample.contains("expected")) {
        std::cerr << "ERROR: malformed sample file\n";
        return 2;
    }
    const auto& rows = sample["rows"];
    const auto& expected = sample["expected"];

    double max_err = 0;
    int64_t total_us = 0;
    size_t n = 0;
    for (size_t i = 0; i < rows.size() && i < expected.size(); ++i) {
        std::vector<double> x;
        x.reserve(rows[i].size());
        for (const auto& v : rows[i]) {
            x.push_back(v.is_null() ? std::nan("") : v.get<double>());
        }
        if (static_cast<int>(x.size()) != n_features) {
            std::cerr << "ERROR: sample row " << i << " has " << x.size() << " features\n";
            return 1;
        }
        double out = 0;
        int64_t out_len = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const int rc = LGBM_BoosterPredictForMatSingleRow(
            booster, x.data(), C_API_DTYPE_FLOAT64, n_features, 1,
            C_API_PREDICT_NORMAL, 0, -1, "", &out_len, &out);
        const auto t1 = std::chrono::steady_clock::now();
        if (rc != 0 || out_len != 1) {
            std::cerr << "ERROR: prediction failed at row " << i << ": "
                      << LGBM_GetLastError() << "\n";
            return 1;
        }
        total_us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        max_err = std::max(max_err, std::abs(out - expected[i].get<double>()));
        ++n;
    }
    LGBM_BoosterFree(booster);

    std::cout << "rows=" << n << " max_abs_error=" << max_err
              << " avg_latency_us=" << (n ? total_us / static_cast<int64_t>(n) : 0) << "\n";
    if (max_err > tolerance) {
        std::cerr << "ERROR: C++/Python prediction mismatch exceeds tolerance " << tolerance
                  << "\n";
        return 1;
    }
    std::cout << "VALIDATION PASSED\n";
    return 0;
}

} // namespace altml::cli
