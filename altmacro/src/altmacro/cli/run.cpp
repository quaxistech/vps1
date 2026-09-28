#include "altmacro/cli/commands.hpp"
#include "altmacro/core/http.hpp"
#include "altmacro/components/supervisor.hpp"
#include "altmacro/core/logger.hpp"
#include "altmacro/core/nats_provision.hpp"

namespace altmacro::cli {

int run_service(const Config& cfg) {
    core::set_default_user_agent(cfg.user_agent);
    components::Supervisor sup(cfg);
    auto r = sup.run();
    if (!r) {
        log::critical("service_failed",
                      {{"error", r.error().message}, {"code", r.error().code_name()}});
        return 1;
    }
    return 0;
}

} // namespace altmacro::cli

namespace altmacro::cli {

int run_nats_setup(const Config& cfg) {
    if (!cfg.nats.enabled) {
        log::warn("nats_disabled", {{"detail", "nothing to provision"}});
        return 0;
    }
    return messaging::nats_provision(cfg.nats.url, cfg.nats) ? 0 : 1;
}

} // namespace altmacro::cli
