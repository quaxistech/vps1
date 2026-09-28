// altml train: one synchronous native training cycle (diagnostics / audit /
// controlled re-run / CI). Normal operation does NOT need this command — the
// in-process trainer schedules itself; this is the manual entry point.
//
//   altml train [--config /etc/altml/altml.yaml] [--force]
//
// --force skips the schedule/resource-pressure checks; every data-quality,
// walk-forward, economic and champion-challenger gate still applies.
// Exit codes: 0 trained, 1 cycle failed, 2 usage/config, 3 deferred (resources).

#include "altml/cli/commands.hpp"

#include "altml/components/trainer.hpp"
#include "altml/core/config.hpp"
#include "altml/core/logger.hpp"
#include "altml/core/shutdown.hpp"

#include <cstdio>
#include <string>

namespace altml::cli {

namespace {

void print_usage() {
    std::puts("usage: altml train [--config /etc/altml/altml.yaml] [--force]\n\n"
              "Runs one bounded native training cycle: load labeled history, walk-forward\n"
              "train/validate, gate, register and (if configured) promote. --force skips\n"
              "only the resource-pressure check, never the safety gates.");
}

} // namespace

int cmd_train(int argc, char** argv) {
    std::string config_path = "/etc/altml/altml.yaml";
    bool force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--force") {
            force = true;
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "altml train: unknown argument '%s'\n", a.c_str());
            print_usage();
            return 2;
        }
    }
    auto rt = config::load_runtime(config_path);
    if (!rt) {
        std::fprintf(stderr, "config error: %s\n", rt.error().message.c_str());
        return 2;
    }
    log::init("altml", rt->common.instance_id, rt->common.log_level);
    log::set_thread_component("train");
    ShutdownToken::instance().install_signal_handlers();
    return run_training_once(*rt, force);
}

} // namespace altml::cli
