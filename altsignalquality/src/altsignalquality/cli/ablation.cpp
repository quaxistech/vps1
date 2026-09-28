// §57 ablation: measure what each input group contributes to signal quality,
// instead of assuming it contributes anything.
//
// Every arm runs the SAME production training cycle -- the same dataset build,
// the same purge and embargo, the same walk-forward folds, the same calibration
// and the same threshold selection -- with one input group blanked. Nothing is
// reimplemented here, so an arm's number and a production model's number mean
// the same thing.
//
// A blanked group is marked ABSENT, not zeroed: the counterfactual being
// measured is "we did not have this input", and a zero would instead assert
// "we had it and it read zero".

#include "altsignalquality/cli/commands.hpp"

#include "altsignalquality/components/trainer.hpp"
#include "altsignalquality/core/config.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/shutdown.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace altsignalquality::cli {
namespace {

void print_usage() {
    std::puts(
        "usage: altsignalquality ablation [--config PATH] [--arm NAME ...] [--all-signals]\n"
        "\n"
        "  Runs the production training cycle once per arm and reports the\n"
        "  metrics side by side. With no --arm, runs the full set:\n"
        "\n"
        "    full            the whole declared contract\n"
        "    no_ml           without AltML evidence (edge_estimated, conviction, ...)\n"
        "    no_cross_venue  without xv_* (Binance / OKX / Bybit agreement)\n"
        "    no_micro        without micro_* (book and tape)\n"
        "    no_htf          without htf_* (higher-timeframe context)\n"
        "    no_regime       without regime_*\n"
        "    no_prior        without prior_* (historical strategy behaviour)\n"
        "\n"
        "  An arm that DEFERS has too little applicable data to choose a\n"
        "  threshold on; that is reported as such, not as a failure.\n"
        "\n"
        "  --all-signals measures on the whole population instead of the\n"
        "  upstream-recommended subset. Production policy is unchanged; this is\n"
        "  for measurement when the recommended subset is too thin to calibrate\n"
        "  on, and every arm run this way is labelled (all-signals).\n");
}

struct Arm {
    const char* name;
    std::vector<std::string> groups;
};

const std::vector<Arm>& all_arms() {
    static const std::vector<Arm> arms = {
        {"full", {}},
        {"no_ml", {"ml"}},
        {"no_cross_venue", {"cross_venue"}},
        {"no_micro", {"microstructure"}},
        {"no_htf", {"htf"}},
        {"no_regime", {"regime"}},
        {"no_prior", {"prior"}},
    };
    return arms;
}

} // namespace

int cmd_ablation(int argc, char** argv) {
    std::string config_path = "/etc/altsignalquality/altsignalquality.yaml";
    std::vector<std::string> wanted;
    bool applicable_all = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--all-signals") {
            applicable_all = true;
        } else if (a == "--arm" && i + 1 < argc) {
            wanted.emplace_back(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "altsignalquality ablation: unknown argument '%s'\n", a.c_str());
            print_usage();
            return 2;
        }
    }
    auto rt = config::load_runtime(config_path);
    if (!rt) {
        std::fprintf(stderr, "config error: %s\n", rt.error().message.c_str());
        return 2;
    }
    log::init("altsignalquality", rt->common.instance_id, rt->common.log_level);
    log::set_thread_component("ablation");
    ShutdownToken::instance().install_signal_handlers();

    std::vector<Arm> arms;
    for (const auto& a : all_arms()) {
        if (wanted.empty()) { arms.push_back(a); continue; }
        for (const auto& w : wanted) if (w == a.name) arms.push_back(a);
    }
    if (arms.empty()) {
        std::fprintf(stderr, "no matching arm; see --help\n");
        return 2;
    }

    std::printf("%-16s %8s %9s %9s %9s %11s %11s  %s\n", "arm", "rows", "roc_auc", "pr_auc",
                "brier", "expectancy", "uplift", "status");
    int failures = 0;
    for (const auto& arm : arms) {
        if (ShutdownToken::instance().stop_requested()) break;
        const auto r = trainer::run_ablation_arm(*rt, arm.groups, applicable_all);
        if (!r) {
            std::printf("%-16s %8s %9s %9s %9s %11s %11s  %s\n", arm.name, "-", "-", "-", "-",
                        "-", "-", r.error().message.c_str());
            // A deferred arm is a data state, not a failure of the arm.
            if (r.error().code != ErrorCode::not_found) ++failures;
            continue;
        }
        std::printf("%-16s %8lld %9.4f %9.4f %9.4f %11.2f %11.2f  %s\n", arm.name,
                    static_cast<long long>(r->rows), r->roc_auc, r->pr_auc, r->brier,
                    r->expectancy_bps, r->expectancy_uplift_bps, r->status.c_str());
    }
    std::puts(
        "\nexpectancy is mean net return per ACCEPTED signal, in bps; uplift is that\n"
        "against the unfiltered population (§56 baseline). A negative uplift means\n"
        "the filter is selecting worse signals than taking them all.");
    return failures == 0 ? 0 : 1;
}

} // namespace altsignalquality::cli
