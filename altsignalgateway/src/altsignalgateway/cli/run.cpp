#include "altsignalgateway/cli/commands.hpp"

#include "altsignalgateway/components/supervisor.hpp"
#include "altsignalgateway/core/config.hpp"
#include "altsignalgateway/core/logger.hpp"

#include <cstdio>
#include <stdexcept>

namespace altsignalgateway::cli {

std::string Args::option(const std::string& name, const std::string& def) const {
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == name && i + 1 < rest.size()) return rest[i + 1];
        const std::string prefix = name + "=";
        if (rest[i].rfind(prefix, 0) == 0) return rest[i].substr(prefix.size());
    }
    return def;
}

int64_t Args::int_option(const std::string& name, int64_t def, bool* ok) const {
    if (ok != nullptr) *ok = true;
    const std::string raw = option(name);
    if (raw.empty()) return def;
    size_t consumed = 0;
    try {
        const int64_t v = std::stoll(raw, &consumed);
        if (consumed != raw.size()) throw std::invalid_argument("trailing characters");
        return v;
    } catch (const std::exception&) {
        std::fprintf(stderr, "%s expects an integer, got \"%s\"\n", name.c_str(), raw.c_str());
        if (ok != nullptr) *ok = false;
        return def;
    }
}

bool Args::flag(const std::string& name) const {
    for (const auto& a : rest) {
        if (a == name) return true;
    }
    return false;
}

int cmd_run(const Args& args) {
    auto cfg = config::load_runtime(args.config);
    if (!cfg) {
        std::fprintf(stderr, "configuration error: %s\n", cfg.error().message.c_str());
        return 1;
    }
    log::init("altsignalgateway", cfg->common.instance_id, cfg->common.log_level);
    log::set_thread_component("supervisor");
    log::info("starting", {{"version", ALTSIGNALGATEWAY_VERSION},
                           {"environment", cfg->common.environment},
                           {"instance", cfg->common.instance_id},
                           {"wiring", cfg->wiring.id},
                           {"config", args.config}});

    auto rc = components::run_service(*cfg, ALTSIGNALGATEWAY_VERSION);
    if (!rc) {
        log::critical("startup_failed", {{"code", rc.error().code_name()},
                                         {"message", rc.error().message}});
        // Also on stderr: a startup failure is frequently read from
        // `systemctl status` before anyone opens the journal.
        std::fprintf(stderr, "altsignalgateway: %s\n", rc.error().message.c_str());
        return 1;
    }
    return *rc;
}

} // namespace altsignalgateway::cli
