// altsignalquality validate: the audit gate.
//
// Checks, without training anything: the feature contract's own leakage
// invariants, that the registered contract matches the compiled one, that the
// upstream AltStrategy tables are readable with the columns this service
// depends on, that the dataset's row and split invariants hold, and that the
// ACTIVE model loads, matches the running contract and still reproduces the
// predictions it recorded at training time.
//
// Prints a JSON report; exit 0 when clean, 1 when anything was found.

#include "altsignalquality/cli/commands.hpp"

#include "altsignalquality/components/trainer.hpp"
#include "altsignalquality/core/config.hpp"
#include "altsignalquality/core/logger.hpp"

#include <cstdio>
#include <string>

namespace altsignalquality::cli {

namespace {

void print_usage() {
    std::puts("usage: altsignalquality validate [--config PATH]\n\n"
              "Audits the feature contract, the dataset, the temporal splits and the ACTIVE\n"
              "model artifact without training. Exit 0 = clean, 1 = findings, 2 = config.");
}

} // namespace

int cmd_validate(int argc, char** argv) {
    std::string config_path = "/etc/altsignalquality/altsignalquality.yaml";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "altsignalquality validate: unknown argument '%s'\n", a.c_str());
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
    log::set_thread_component("validate");
    return run_validation(*rt);
}

} // namespace altsignalquality::cli
