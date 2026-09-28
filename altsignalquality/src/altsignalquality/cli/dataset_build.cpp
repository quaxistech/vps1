// altsignalquality dataset-build: one synchronous dataset pass.
//
// Folds newly matured AltStrategy outcomes into the as-of priors and turns
// matured signals into immutable training examples, then reports the counts.
// The service does this on its own schedule; this command exists for catching
// up after an outage, for CI, and for making the dataset state legible.

#include "altsignalquality/cli/commands.hpp"

#include "altsignalquality/components/dataset.hpp"
#include "altsignalquality/core/config.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/shutdown.hpp"

#include <cstdio>
#include <string>

namespace altsignalquality::cli {

namespace {

void print_usage() {
    std::puts("usage: altsignalquality dataset-build [--config PATH]\n\n"
              "Refreshes the as-of outcome priors and materialises training examples for\n"
              "every matured signal that does not have one under the current contracts.");
}

} // namespace

int cmd_dataset_build(int argc, char** argv) {
    std::string config_path = "/etc/altsignalquality/altsignalquality.yaml";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "altsignalquality dataset-build: unknown argument '%s'\n",
                         a.c_str());
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
    log::set_thread_component("dataset");
    ShutdownToken::instance().install_signal_handlers();
    return run_dataset_once(*rt);
}

} // namespace altsignalquality::cli
