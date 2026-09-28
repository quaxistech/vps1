// altml nats-setup: manual/CI entry point for JetStream provisioning.
//
//   altml nats-setup [--url nats://...] [--config /etc/altml/altml.yaml]
//
// With --config the project adapter section defines the topology; otherwise
// the built-in defaults (the deployed Bitget contract) are used. URL
// resolution order: --url, config nats_url, ALTML_NATS_URL.
//
// `altml run` performs the same provisioning inline at startup; this
// subcommand exists for operators and CI. Non-zero exit on any failure.

#include "altml/cli/commands.hpp"

#include "altml/core/config.hpp"
#include "altml/core/logger.hpp"
#include "altml/core/nats_provision.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace altml::cli {

namespace {

void print_usage() {
    std::puts("usage: altml nats-setup [--url NATS_URL] [--config /etc/altml/altml.yaml]\n\n"
              "Provisions/validates the project's JetStream streams and durable.");
}

} // namespace

int cmd_nats_setup(int argc, char** argv) {
    std::string url;
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--url" && i + 1 < argc) {
            url = argv[++i];
        } else if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "altml nats-setup: unknown argument '%s'\n", a.c_str());
            print_usage();
            return 2;
        }
    }

    config::ProjectConfig project; // defaults = deployed Bitget topology
    if (!config_path.empty()) {
        auto rt = config::load_runtime(config_path);
        if (!rt) {
            std::fprintf(stderr, "config error: %s\n", rt.error().message.c_str());
            return 2;
        }
        project = rt->project;
        if (url.empty()) url = rt->common.nats_url;
    }
    if (url.empty()) {
        if (const char* env = std::getenv("ALTML_NATS_URL")) url = env;
    }

    log::init("altml", "cli", "info");
    log::set_thread_component("nats-setup");
    if (url.empty()) {
        log::error("nats_init_config_error",
                   {{"message", "no URL: use --url, --config or ALTML_NATS_URL"}});
        return 2;
    }

    return messaging::nats_provision(url, project) ? 0 : 1;
}

} // namespace altml::cli
