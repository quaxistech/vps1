#include "altstrategy/config/service_bootstrap.hpp"

#include "altstrategy/common/shutdown.hpp"
#include "altstrategy/logging/logger.hpp"

#include <iostream>

namespace altstrategy::config {

Result<ServiceContext> bootstrap_service(const std::string& service_name, int argc, char** argv) {
    std::string config_path;
    std::string common_path = "/etc/altstrategy/common.yaml";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--common" && i + 1 < argc) {
            common_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: " << service_name
                      << " --config <service.yaml> [--common <common.yaml>]\n";
            std::exit(0);
        } else {
            return fail(ErrorCode::config_error, "unknown argument: " + arg);
        }
    }
    if (config_path.empty()) {
        return fail(ErrorCode::config_error, "--config is required");
    }

    auto common = load_common(common_path);
    if (!common) return std::unexpected(common.error());
    auto common_raw = load_yaml(common_path);
    if (!common_raw) return std::unexpected(common_raw.error());

    auto service = load_yaml(config_path);
    if (!service) return std::unexpected(service.error());

    log::init(service_name, common->instance_id, common->log_level);
    ShutdownToken::instance().install_signal_handlers();

    log::info("service_starting", {{"environment", common->environment},
                                   {"config", config_path}});

    ServiceContext ctx;
    ctx.common = std::move(*common);
    ctx.common_yaml = std::move(*common_raw);
    ctx.service = std::move(*service);
    ctx.service_name = service_name;
    return ctx;
}

} // namespace altstrategy::config
