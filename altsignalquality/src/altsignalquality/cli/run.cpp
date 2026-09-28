// altsignalquality run: the unified service process.
//
// Provisions the JetStream topology inline, then runs the evaluator, dataset,
// trainer and model-registry components as supervised threads. One SIGTERM
// stops everything; one failed component stops the whole process (exit 1) so
// systemd restarts it as a unit rather than leaving a half-working filter.

#include "altsignalquality/cli/commands.hpp"

#include "altsignalquality/components/dataset.hpp"
#include "altsignalquality/components/evaluator.hpp"
#include "altsignalquality/components/model_registry.hpp"
#include "altsignalquality/components/supervisor.hpp"
#include "altsignalquality/components/trainer.hpp"
#include "altsignalquality/core/config.hpp"
#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/nats_client.hpp"
#include "altsignalquality/core/nats_provision.hpp"
#include "altsignalquality/core/shutdown.hpp"
#include "altsignalquality/domain/feature_contract.hpp"

#include <cstdio>
#include <memory>
#include <string>

namespace altsignalquality::cli {

namespace {

void print_usage() {
    std::puts("usage: altsignalquality run [--config /etc/altsignalquality/altsignalquality.yaml]"
              "\n\n"
              "Runs the evaluator, dataset, trainer and model-registry components in one\n"
              "supervised process. JetStream is provisioned before startup; with no ACTIVE\n"
              "model the evaluator serves under the configured cold-start policy while the\n"
              "trainer accumulates history and bootstraps a model on its own.");
}

} // namespace

int cmd_run(int argc, char** argv) {
    std::string config_path = "/etc/altsignalquality/altsignalquality.yaml";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "altsignalquality run: unknown argument '%s'\n", a.c_str());
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
    log::set_thread_component("supervisor");
    ShutdownToken::instance().install_signal_handlers();
    log::info("service_starting",
              {{"environment", rt->common.environment},
               {"wiring", rt->wiring.id},
               {"objective", rt->wiring.objective},
               {"feature_schema", std::string(domain::kFeatureSchemaVersion)},
               {"feature_schema_hash", domain::feature_schema_hash()},
               {"config", config_path}});

    // 1. JetStream first: the input durable and the output stream must exist
    //    before any component touches NATS.
    if (!messaging::nats_provision(rt->common.nats_url, rt->wiring)) {
        log::critical("nats_provision_failed", {});
        return 1;  // systemd Restart=on-failure retries within the start limit
    }

    // 2. One shared thread-safe NATS client; each component owns its own PgPool.
    auto nats = messaging::NatsClient::connect(rt->common.nats_url, "altsignalquality");
    if (!nats) {
        log::critical("nats_unavailable", {{"message", nats.error().message}});
        return 1;
    }

    Supervisor supervisor;
    supervisor.add(std::make_unique<EvaluatorComponent>(*rt, **nats));
    supervisor.add(std::make_unique<DatasetComponent>(*rt));
    supervisor.add(std::make_unique<TrainerComponent>(*rt));
    supervisor.add(std::make_unique<ModelRegistryComponent>(*rt, **nats));
    if (auto r = supervisor.init_all(); !r) {
        return r.error().code == ErrorCode::config_error ? 2 : 1;
    }

    const int rc = supervisor.run_all();  // READY=1 inside, after threads spawn
    (*nats)->drain(3000);
    log::info("service_stopped", {{"exit_code", rc}});
    return rc;
}

} // namespace altsignalquality::cli
