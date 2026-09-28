// altstrategy-config-check: validates all AltStrategy YAML configs without printing secrets.
//
//   altstrategy-config-check --dir /etc/altstrategy      (validates the standard set)
//   altstrategy-config-check --file <path> --kind <k>  (single AltStrategy-owned config)
//
// Exit codes: 0 = all valid, 1 = validation errors, 2 = usage error.

#include "altstrategy/config/config.hpp"
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using altstrategy::config::load_yaml;

namespace {

struct CheckResult {
    std::string file;
    bool ok = true;
    std::vector<std::string> errors;
};

void require(CheckResult& r, bool cond, const std::string& message) {
    if (!cond) {
        r.ok = false;
        r.errors.push_back(message);
    }
}

CheckResult check_common(const std::string& path) {
    CheckResult r;
    r.file = path;
    auto c = altstrategy::config::load_common(path);
    require(r, c.has_value(), c ? "" : c.error().message);
    return r;
}

CheckResult check_service(const std::string& path, const std::string& kind) {
    CheckResult r;
    r.file = path;
    auto doc = load_yaml(path);
    if (!doc) {
        require(r, false, doc.error().message);
        return r;
    }
    const YAML::Node& n = *doc;

    if (kind == "exchange-simulator") {
        const int64_t fill_bps = altstrategy::config::opt_int(n, "default_fill_bps", 10'000);
        const int64_t portfolio_age =
            altstrategy::config::opt_int(n, "portfolio_snapshot_max_age_sec", 30);
        const int64_t market_age = altstrategy::config::opt_int(n, "market_data_max_age_sec", 90);
        require(r, fill_bps == 10'000,
                "default_fill_bps must be 10000 until partial-fill accounting is implemented");
        require(r, !altstrategy::config::opt_str(n, "shadow_account_id", "default").empty(),
                "shadow_account_id must not be empty");
        require(r, portfolio_age > 0,
                "portfolio_snapshot_max_age_sec must be positive");
        require(r, market_age > 0, "market_data_max_age_sec must be positive");
    }

    const int64_t pool = altstrategy::config::opt_int(n, "postgres_pool_size", 2);
    require(r, pool >= 1 && pool <= 16, "postgres_pool_size out of range [1, 16]");
    return r;
}

} // namespace

int main(int argc, char** argv) {
    std::string dir;
    std::string file;
    std::string kind;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (arg == "--file" && i + 1 < argc) {
            file = argv[++i];
        } else if (arg == "--kind" && i + 1 < argc) {
            kind = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: altstrategy-config-check --dir /etc/altstrategy | --file F --kind K\n";
            return 0;
        }
    }

    std::vector<CheckResult> results;
    if (!file.empty()) {
        if (kind.empty()) {
            std::cerr << "--kind is required with --file\n";
            return 2;
        }
        results.push_back(kind == "common" ? check_common(file) : check_service(file, kind));
    } else if (!dir.empty()) {
        const std::vector<std::pair<std::string, std::string>> files = {
            {"common.yaml", "common"},
            {"strategy-engine.yaml", "generic"},
            {"strategy-ranking.yaml", "generic"},
            {"trade-plan.yaml", "generic"},
            {"signal-orchestrator.yaml", "generic"},
            {"signal-outcome.yaml", "generic"},
            {"trading-control.yaml", "generic"},
            {"risk-engine.yaml", "generic"},
            {"portfolio-risk.yaml", "generic"},
            {"execution-orchestrator.yaml", "generic"},
            {"bitget-execution-gateway.yaml", "generic"},
            {"order-management.yaml", "generic"},
            {"position-lifecycle.yaml", "generic"},
            {"reconciliation.yaml", "generic"},
            {"pnl-accounting.yaml", "generic"},
            {"exchange-health.yaml", "generic"},
            {"notification.yaml", "generic"},
            {"exchange-simulator.yaml", "exchange-simulator"},
        };
        for (const auto& [fname, k] : files) {
            const fs::path p = fs::path(dir) / fname;
            results.push_back(k == "common" ? check_common(p.string())
                                            : check_service(p.string(), k));
        }
    } else {
        std::cerr << "usage: altstrategy-config-check --dir /etc/altstrategy | --file F --kind K\n";
        return 2;
    }

    bool all_ok = true;
    for (const auto& r : results) {
        if (r.ok) {
            std::cout << "OK    " << r.file << "\n";
        } else {
            all_ok = false;
            std::cout << "ERROR " << r.file << "\n";
            for (const auto& e : r.errors) {
                if (!e.empty()) std::cout << "      - " << e << "\n";
            }
        }
    }
    return all_ok ? 0 : 1;
}
