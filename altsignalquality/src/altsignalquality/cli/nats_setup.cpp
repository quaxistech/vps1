// altsignalquality nats-setup: manual JetStream provisioning.
//
// The same code path `run` executes inline at startup, exposed so a deployment
// can provision (and validate the AltStrategy stream contract) before the
// service is ever started.

#include "altsignalquality/cli/commands.hpp"

#include "altsignalquality/core/config.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/nats_provision.hpp"
#include "altsignalquality/core/shutdown.hpp"

#include <cstdio>
#include <string>

namespace altsignalquality::cli {

namespace {

void print_usage() {
    std::puts("usage: altsignalquality nats-setup [--config PATH]\n\n"
              "Creates the AltSignalQuality output stream and the durable consumer on\n"
              "AltStrategy's decision stream, and validates that stream's contract.\n"
              "Idempotent; never creates or reconfigures another service's stream.");
}

} // namespace

int cmd_nats_setup(int argc, char** argv) {
    std::string config_path = "/etc/altsignalquality/altsignalquality.yaml";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "altsignalquality nats-setup: unknown argument '%s'\n",
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
    log::set_thread_component("nats-setup");
    ShutdownToken::instance().install_signal_handlers();
    return messaging::nats_provision(rt->common.nats_url, rt->wiring) ? 0 : 1;
}

} // namespace altsignalquality::cli
