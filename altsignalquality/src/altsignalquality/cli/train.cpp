// altsignalquality train: one synchronous training cycle (diagnostics / audit /
// controlled re-run / CI). Normal operation does NOT need this command — the
// in-process trainer schedules itself.
//
//   altsignalquality train [--config PATH] [--force]
//
// --force skips only the schedule and resource-pressure checks; every leakage,
// data-quality, walk-forward, calibration and promotion gate still applies.

#include "altsignalquality/cli/commands.hpp"

#include "altsignalquality/components/trainer.hpp"
#include "altsignalquality/core/config.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/shutdown.hpp"

#include <cstdio>
#include <string>

namespace altsignalquality::cli {

namespace {

void print_usage() {
    std::puts("usage: altsignalquality train [--config PATH] [--force]\n\n"
              "Runs one bounded training cycle: load matured signals, audit for leakage,\n"
              "walk-forward train/calibrate/threshold, gate against 'accept every signal',\n"
              "compare with the incumbent, register and (if configured) promote.\n"
              "Exit codes: 0 promoted/approved, 1 rejected or failed, 2 config, 3 deferred.");
}

} // namespace

int cmd_train(int argc, char** argv) {
    std::string config_path = "/etc/altsignalquality/altsignalquality.yaml";
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
            std::fprintf(stderr, "altsignalquality train: unknown argument '%s'\n", a.c_str());
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
    log::set_thread_component("train");
    ShutdownToken::instance().install_signal_handlers();
    return run_training_once(*rt, force);
}

} // namespace altsignalquality::cli
