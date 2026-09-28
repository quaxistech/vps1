#pragma once

#include "altmacro/core/config.hpp"
#include "altmacro/core/error.hpp"
#include "altmacro/data/store.hpp"
#include "altmacro/providers/provider.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace altmacro::providers {

// Builds the live provider objects from configuration and the database
// registry, and resolves each instrument to the source that will serve it.
class Registry {
public:
    [[nodiscard]] static Result<Registry>
    build(const Config& cfg, const std::vector<data::SourceBinding>& bindings);

    [[nodiscard]] ProviderPtr get(const std::string& provider_code) const;

    // Bindings the service will actually work on: enabled, with a provider that
    // exists, and — for scaled-integer feeds — with a verified decode scale.
    [[nodiscard]] const std::vector<data::SourceBinding>& active() const { return active_; }

    // Bindings that were configured but cannot be served, with the reason.
    // Reported at startup so an instrument never disappears silently.
    struct Rejection { std::string instrument_code; std::string provider_code; std::string reason; };
    [[nodiscard]] const std::vector<Rejection>& rejected() const { return rejected_; }

private:
    std::map<std::string, ProviderPtr> providers_;
    std::vector<data::SourceBinding> active_;
    std::vector<Rejection> rejected_;
};

} // namespace altmacro::providers
