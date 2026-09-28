// altml run: the unified AltML service process.
//
// Provisions the project's JetStream topology inline, then runs the
// label-builder, model-registry and inference components as supervised
// threads. One SIGTERM stops everything; one failed component stops the whole
// process (exit 1) so systemd restarts it as a unit.

#include "altml/cli/commands.hpp"

#include "altml/components/inference.hpp"
#include "altml/components/label_builder.hpp"
#include "altml/components/model_registry.hpp"
#include "altml/components/storage_guard.hpp"
#include "altml/components/supervisor.hpp"
#include "altml/components/trainer.hpp"
#include "altml/core/config.hpp"
#include "altml/core/logger.hpp"
#include "altml/core/nats_client.hpp"
#include "altml/core/nats_provision.hpp"
#include "altml/core/shutdown.hpp"

#include <cstdio>
#include <memory>
#include <string>

namespace altml::cli {

namespace {

void print_usage() {
    std::puts("usage: altml run [--config /etc/altml/altml.yaml]\n\n"
              "Runs the label-builder, trainer, model-registry and inference components\n"
              "in one supervised process. JetStream topology is provisioned before\n"
              "startup; the trainer bootstraps a model automatically when none is ACTIVE.");
}

} // namespace

int cmd_run(int argc, char** argv) {
    std::string config_path = "/etc/altml/altml.yaml";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "altml run: unknown argument '%s'\n", a.c_str());
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
    log::set_thread_component("supervisor");
    ShutdownToken::instance().install_signal_handlers();
    log::info("service_starting", {{"environment", rt->common.environment},
                                   {"project", rt->project.id},
                                   {"config", config_path}});

    // 1. Inline JetStream provisioning — same code as `altml nats-setup`.
    //    The control stream and the inference durable exist before any
    //    component touches NATS (the ordering the old oneshot unit provided).
    if (!messaging::nats_provision(rt->common.nats_url, rt->project)) {
        log::critical("nats_provision_failed", {});
        return 1; // systemd Restart=on-failure retries within the start limit
    }

    // 2. One shared thread-safe NATS client; PgPools live inside components.
    auto nats = messaging::NatsClient::connect(rt->common.nats_url, "altml");
    if (!nats) {
        log::critical("nats_unavailable", {{"message", nats.error().message}});
        return 1;
    }

    Supervisor supervisor;
    supervisor.add(std::make_unique<LabelBuilderComponent>(*rt));
    supervisor.add(std::make_unique<TrainerComponent>(*rt));
    supervisor.add(std::make_unique<ModelRegistryComponent>(*rt, **nats));
    supervisor.add(std::make_unique<InferenceComponent>(*rt, **nats));
    supervisor.add(std::make_unique<StorageGuardComponent>(*rt));
    if (auto r = supervisor.init_all(); !r) {
        return r.error().code == ErrorCode::config_error ? 2 : 1;
    }

    const int rc = supervisor.run_all(); // READY=1 inside, after threads spawn
    (*nats)->drain(3000);                // single drain after all components joined
    log::info("service_stopped", {{"exit_code", rc}});
    return rc;
}

} // namespace altml::cli
