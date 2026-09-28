#pragma once

#include "altmacro/core/http.hpp"
#include "altmacro/core/rate_limiter.hpp"
#include "altmacro/providers/provider.hpp"

#include <string>
#include <vector>

namespace altmacro::providers {

// Frankfurter v2 — an aggregator over 97 official sources, each of which
// states its own rate_type ("central parity rate", "H.10", "reference rate",
// "PTAX closing", and so on).
//
// The one rule that matters here: this provider is NEVER queried without an
// explicit `providers=` filter. The unfiltered endpoint returns a blend across
// all 97 sources; that blend is nobody's published rate, and storing it as
// though it were an authority's number would be exactly the kind of quiet
// semantic corruption this service exists to prevent.
//
// A symbol is therefore written as PROVIDER:BASE:QUOTE, e.g. "CFETS:USD:CNY".
// All three parts are required, so a binding cannot accidentally lose the
// authority it came from.
//
// This is the historical path for the PBOC fixing: CFETS covers 2006-01-04
// onward, whereas chinamoney.com.cn — the first-hand publisher — serves only
// the current day. The two were checked against each other on the same date
// and agreed exactly.
//
// Frankfurter documents no numeric quota: "There are no quotas. Requests are
// rate-limited to prevent abuse, but there are no monthly or daily caps." The
// pacing below is therefore ours, not theirs.
class FrankfurterProvider final : public Provider {
public:
    FrankfurterProvider();

    [[nodiscard]] const ProviderMetadata& metadata() const override { return meta_; }

    // Reads /v2/providers, which publishes each source's key, rate_type,
    // pivot_currency, coverage dates and currency list. That is a real
    // catalogue, so discovery reads it rather than probing.
    [[nodiscard]] Result<std::vector<InstrumentDescriptor>>
    discover_instruments(const ShouldStop& should_stop) override;

    [[nodiscard]] Result<ReferencePage>
    fetch_historical_reference(const InstrumentDescriptor& instrument, Window window,
                               const ShouldStop& should_stop) override;

    [[nodiscard]] Result<ReferencePage>
    fetch_latest(const InstrumentDescriptor& instrument, const ShouldStop& should_stop) override;

    [[nodiscard]] Result<Health> health(const ShouldStop& should_stop) override;

    // Restricts discovery to the authorities actually wanted, e.g. {"CFETS","ECB"}.
    void set_wanted_sources(std::vector<std::string> source_keys);

    struct Binding { std::string source; std::string base; std::string quote; };
    [[nodiscard]] static Result<Binding> parse_symbol(const std::string& native_symbol);

private:
    ProviderMetadata meta_;
    std::vector<std::string> wanted_;
    core::TokenBucket bucket_;

    [[nodiscard]] Result<core::HttpResponse>
    fetch(const std::string& target, const ShouldStop& should_stop);
};

} // namespace altmacro::providers
