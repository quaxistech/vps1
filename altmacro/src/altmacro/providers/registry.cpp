#include "altmacro/providers/registry.hpp"

#include "altmacro/core/logger.hpp"
#include "altmacro/providers/dukascopy.hpp"
#include "altmacro/providers/fred.hpp"
#include "altmacro/providers/frankfurter.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace altmacro::providers {

Result<Registry> Registry::build(const Config& cfg,
                                 const std::vector<data::SourceBinding>& bindings) {
    Registry reg;

    // Only providers that some binding actually needs are constructed, so an
    // unused source cannot fail startup.
    std::set<std::string> needed;
    for (const auto& b : bindings) {
        if (b.enabled) needed.insert(b.provider_code);
    }

    for (const auto& code : needed) {
        // Configuration may switch a provider off without touching the
        // registry; the bindings that depended on it are then reported as
        // rejected rather than quietly dropped.
        if (auto it = cfg.providers.find(code); it != cfg.providers.end() && !it->second.enabled) {
            continue;
        }

        ProviderPtr p;
        if (code == "dukascopy") {
            auto d = std::make_shared<DukascopyProvider>();
            std::vector<std::string> symbols;
            for (const auto& b : bindings) {
                if (b.provider_code != code || !b.enabled) continue;
                if (!b.decode_scale || !b.sanity_min || !b.sanity_max) continue;
                d->set_decode(b.native_symbol, *b.decode_scale, *b.sanity_min, *b.sanity_max);
                if (std::find(symbols.begin(), symbols.end(), b.native_symbol) == symbols.end()) {
                    symbols.push_back(b.native_symbol);
                }
            }
            d->set_candidate_symbols(std::move(symbols));
            p = d;
        } else if (code == "fred" || code == "alfred") {
            auto f = std::make_shared<FredProvider>(code == "alfred");
            std::vector<std::string> series;
            for (const auto& b : bindings) {
                if (b.provider_code == code && b.enabled) series.push_back(b.native_symbol);
            }
            f->set_series(std::move(series));
            p = f;
        } else if (code == "frankfurter") {
            auto f = std::make_shared<FrankfurterProvider>();
            std::vector<std::string> sources;
            for (const auto& b : bindings) {
                if (b.provider_code != code || !b.enabled) continue;
                auto parsed = FrankfurterProvider::parse_symbol(b.native_symbol);
                if (!parsed) return std::unexpected(parsed.error());
                if (std::find(sources.begin(), sources.end(), parsed->source) == sources.end()) {
                    sources.push_back(parsed->source);
                }
            }
            f->set_wanted_sources(std::move(sources));
            p = f;
        } else {
            // A registry row naming a provider this binary does not implement
            // is a configuration mistake worth stating, not a silent skip.
            log::warn("provider_not_implemented",
                      {{"provider", code},
                       {"note", "seeded in the registry but not built into this binary"}});
            continue;
        }

        // A provider needing a credential gets it from the environment or is
        // left out. The value is never logged and never stored.
        if (p->metadata().access == AccessMode::api_key_required) {
            const auto& var = p->metadata().credential_env_var;
            if (!var || std::getenv(var->c_str()) == nullptr) {
                log::info("provider_disabled_no_credential",
                          {{"provider", code},
                           {"expected_env_var", var.value_or("(unspecified)")}});
                continue;
            }
        }

        reg.providers_[code] = std::move(p);
    }

    for (const auto& b : bindings) {
        if (!b.enabled) {
            reg.rejected_.push_back({b.instrument_code, b.provider_code, "disabled in the registry"});
            continue;
        }
        if (reg.providers_.find(b.provider_code) == reg.providers_.end()) {
            reg.rejected_.push_back({b.instrument_code, b.provider_code,
                                     "provider unavailable: not implemented, switched off, or "
                                     "missing its credential"});
            continue;
        }
        if (b.provider_code == "dukascopy" && !b.decode_scale) {
            // The one rejection that is a safety property rather than a
            // configuration gap: without a verified scale the prices would be
            // wrong by a factor of ten and look entirely plausible.
            reg.rejected_.push_back({b.instrument_code, b.provider_code,
                                     "no verified decode scale; refusing to guess one"});
            continue;
        }
        reg.active_.push_back(b);
    }

    return reg;
}

ProviderPtr Registry::get(const std::string& provider_code) const {
    const auto it = providers_.find(provider_code);
    return it == providers_.end() ? nullptr : it->second;
}

} // namespace altmacro::providers
