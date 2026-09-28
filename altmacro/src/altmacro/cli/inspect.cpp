#include "altmacro/cli/commands.hpp"
#include "altmacro/core/http.hpp"
#include "altmacro/core/logger.hpp"
#include "altmacro/core/pg.hpp"
#include "altmacro/core/time.hpp"
#include "altmacro/data/store.hpp"
#include "altmacro/providers/registry.hpp"

#include <cstdlib>
#include <iostream>

namespace altmacro::cli {
namespace {

std::string when(const std::optional<int64_t>& ms) {
    return ms ? timeutil::to_iso8601(*ms).substr(0, 19) : std::string("-");
}

} // namespace

int run_inspect(const Config& cfg, const std::string& what) {
    core::set_default_user_agent(cfg.user_agent);
    const char* pw = std::getenv("ALTMACRO_DB_PASSWORD");
    if (pw == nullptr) {
        std::cerr << "ALTMACRO_DB_PASSWORD is not set\n";
        return 2;
    }
    persistence::PgPool pool(cfg.dsn(pw), cfg.database.pool_size);
    data::Store store(pool);

    auto bindings = store.load_bindings();
    if (!bindings) {
        std::cerr << "registry: " << bindings.error().message << "\n";
        return 1;
    }

    if (what == "registry" || what == "all") {
        std::cout << "\ninstrument                provider     symbol           basis  class"
                     "               scale     window\n";
        for (const auto& b : *bindings) {
            std::printf("%-25s %-12s %-16s %-6s %-19s %-9s %s\n",
                        b.instrument_code.c_str(), b.provider_code.c_str(),
                        b.native_symbol.c_str(), domain::to_string(b.price_basis),
                        b.series_class.c_str(),
                        b.decode_scale ? std::to_string(*b.decode_scale).c_str() : "-",
                        b.denomination.c_str());
        }
    }

    if (what == "coverage" || what == "all") {
        auto cov = store.coverage_report();
        if (!cov) {
            std::cerr << "coverage: " << cov.error().message << "\n";
            return 1;
        }
        std::cout << "\ninstrument                provider     tf     basis      rows      valid"
                     "     closed    missing   from                 to\n";
        for (const auto& c : *cov) {
            std::printf("%-25s %-12s %-6s %-10s %-9lld %-9lld %-9lld %-9lld %-20s %s\n",
                        c.instrument_code.c_str(), c.provider_code.c_str(), c.timeframe.c_str(),
                        c.price_basis.c_str(), static_cast<long long>(c.rows),
                        static_cast<long long>(c.valid), static_cast<long long>(c.market_closed),
                        static_cast<long long>(c.missing), when(c.earliest_ms).c_str(),
                        when(c.latest_ms).c_str());
        }
    }

    if (what == "providers" || what == "all") {
        auto registry = providers::Registry::build(cfg, *bindings);
        if (!registry) {
            std::cerr << "providers: " << registry.error().message << "\n";
            return 1;
        }
        std::cout << "\nactive bindings: " << registry->active().size()
                  << ", not served: " << registry->rejected().size() << "\n";
        for (const auto& r : registry->rejected()) {
            std::printf("  %-25s %-12s %s\n", r.instrument_code.c_str(), r.provider_code.c_str(),
                        r.reason.c_str());
        }
    }
    return 0;
}

} // namespace altmacro::cli
