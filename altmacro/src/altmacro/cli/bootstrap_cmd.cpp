#include "altmacro/cli/commands.hpp"
#include "altmacro/core/http.hpp"
#include "altmacro/components/bootstrap.hpp"
#include "altmacro/components/gap_detector.hpp"
#include "altmacro/core/logger.hpp"
#include "altmacro/core/shutdown.hpp"
#include "altmacro/core/time.hpp"

#include <cstdlib>
#include <iostream>

namespace altmacro::cli {

int run_bootstrap(const Config& cfg, bool scan_gaps) {
    core::set_default_user_agent(cfg.user_agent);
    auto& shutdown = ShutdownToken::instance();
    shutdown.install_signal_handlers();
    const auto should_stop = [&] { return shutdown.stop_requested(); };

    const char* pw = std::getenv("ALTMACRO_DB_PASSWORD");
    if (pw == nullptr) {
        log::critical("missing_db_password",
                      {{"detail", "ALTMACRO_DB_PASSWORD is not set"}});
        return 2;
    }

    persistence::PgPool pool(cfg.dsn(pw), cfg.database.pool_size);
    data::Store store(pool);

    auto bindings = store.load_bindings();
    if (!bindings) {
        log::critical("registry_load_failed", {{"error", bindings.error().message}});
        return 1;
    }
    auto registry = providers::Registry::build(cfg, *bindings);
    if (!registry) {
        log::critical("registry_build_failed", {{"error", registry.error().message}});
        return 1;
    }

    components::Bootstrap boot(cfg, *registry, store);
    auto summary = boot.run(should_stop);
    if (!summary) {
        log::critical("bootstrap_failed", {{"error", summary->results.size()}});
        return 1;
    }

    std::cout << "\ninstrument                provider     basis  rows       chunks  closed  "
                 "stale      rejected  outcome\n";
    std::cout << "------------------------------------------------------------------------"
                 "-----------------------\n";
    for (const auto& r : summary->results) {
        std::printf("%-25s %-12s %-6s %-10lld %-7lld %-7lld %-10lld %-9lld %s%s%s\n",
                    r.instrument_code.c_str(), r.provider_code.c_str(), r.price_basis.c_str(),
                    static_cast<long long>(r.rows_written), static_cast<long long>(r.chunks_done),
                    static_cast<long long>(r.chunks_closed),
                    static_cast<long long>(r.stale_rows),
                    static_cast<long long>(r.rejected_rows), r.outcome.c_str(),
                    r.detail.empty() ? "" : " — ", r.detail.c_str());
    }
    std::printf("\ntotal rows written: %lld%s\n", static_cast<long long>(summary->total_rows),
                summary->interrupted ? "  (interrupted; rerun to continue)" : "");

    if (scan_gaps && !summary->interrupted) {
        const auto from = timeutil::parse_iso8601(cfg.bootstrap.history_start);
        const int64_t to = cfg.bootstrap.history_end.empty()
                               ? timeutil::now_ms()
                               : timeutil::parse_iso8601(cfg.bootstrap.history_end).value_or(
                                     timeutil::now_ms());
        if (from) {
            components::GapDetector detector(store);
            auto findings = detector.scan(registry->active(), *from, to);
            if (!findings) {
                log::error("gap_scan_failed", {{"error", findings.error().message}});
                return 1;
            }
            int64_t missing = 0;
            for (const auto& f : *findings) {
                if (f.classification == "data_missing") ++missing;
            }
            std::printf("gap scan: %zu findings, %lld classified data_missing\n",
                        findings->size(), static_cast<long long>(missing));
        }
    }
    // Exit codes are what systemd and an operator actually read:
    //   0  every servable series reached the end of the window
    //   3  interrupted; rerun to continue
    //   4  finished, but at least one series is incomplete or failed
    // Returning 0 for the third case is how a one-third-loaded series gets
    // recorded as a clean one-shot and never looked at again.
    if (summary->interrupted) return 3;
    return summary->complete() ? 0 : 4;
}

} // namespace altmacro::cli
