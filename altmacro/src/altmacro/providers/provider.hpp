#pragma once

#include "altmacro/core/error.hpp"
#include "altmacro/domain/series.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace altmacro::providers {

using domain::PriceBar;
using domain::PriceBasis;
using domain::ReferenceValue;
using domain::ReleaseObservation;
using domain::SeriesClass;
using domain::TimestampSemantics;

// How the provider is reached. Recorded so an operator can see at a glance
// which series stop if a credential is withdrawn. Secrets themselves are read
// from the environment at run time and never stored, logged or printed.
enum class AccessMode { keyless, api_key_required };

// What a provider can actually do. Declared rather than assumed, because the
// bootstrap plans its work from this: a daily-only source must never be asked
// for minutes, and must never be presented as intraday.
struct Capabilities {
    bool historical = false;
    bool latest = false;
    bool intraday = false;          // minute or finer
    bool vintages = false;          // can answer "as known on date D"
    bool bid_ask = false;           // publishes both sides, not just a mid
    // Coarsest-to-finest timeframes the provider genuinely serves.
    std::vector<std::string> timeframes;
    // Politeness budget, from the provider's stated terms where they exist and
    // from observed throttling where they do not.
    int32_t rate_limit_per_min = 30;
    std::optional<int32_t> daily_request_budget;
};

// One instrument in the provider's own namespace, as read from the provider's
// catalogue or verified against it. Never guessed: an unverified ticker that
// happens to return bytes is the fastest way to store one country's currency
// under another's name.
struct InstrumentDescriptor {
    std::string native_symbol;
    std::string display_name;
    SeriesClass series_class = SeriesClass::market;
    // The denomination exactly as the source publishes it, e.g. "CNY per EUR".
    // Stored verbatim; rewriting it would silently change economic meaning.
    std::string denomination;
    std::string unit;                 // price_ratio, percent_per_annum, index_points, ...
    std::string native_frequency;     // tick, minute, daily, monthly, ...
    PriceBasis price_basis = PriceBasis::mid;
    TimestampSemantics timestamp_semantics = TimestampSemantics::point_in_time;
    std::string source_timezone = "UTC";
    std::string session_calendar = "none";
    // First instant the provider actually serves for this symbol, discovered by
    // probing rather than assumed. Nothing before it is ever synthesised.
    std::optional<int64_t> earliest_available_ms;
};

// A closed request window. Providers that page internally do so behind this.
struct Window {
    int64_t start_ms = 0;   // inclusive
    int64_t end_ms = 0;     // exclusive
};

// What a fetch returned. A provider reports emptiness with a reason so that
// the caller can tell "the market was shut" from "the provider broke" without
// inspecting HTTP codes it should not know about.
// Why a page came back with nothing in it. The distinction is load-bearing:
// "the market was shut" and "the archive has not been written yet" look
// identical from the caller's side and mean opposite things to a consumer.
enum class AbsenceReason {
    none,                 // the page carries data
    market_closed,        // the source served the period and it holds no quotes
    not_in_archive_yet,   // the period is current or future; the file will appear
    absent_at_source,     // a past period the source simply does not have
};

struct BarPage {
    std::vector<PriceBar> bars;
    // Set when the provider knows more data exists beyond what it returned.
    std::optional<int64_t> next_cursor_ms;
    AbsenceReason absence = AbsenceReason::none;
};

struct ReferencePage {
    std::vector<ReferenceValue> values;
    std::optional<int64_t> next_cursor_ms;
    AbsenceReason absence = AbsenceReason::none;
};

struct ReleasePage {
    std::vector<ReleaseObservation> observations;
    std::optional<int64_t> next_cursor_ms;
};

// Observed provider health, kept by the caller and handed back for reporting.
struct Health {
    bool reachable = false;
    int32_t consecutive_failures = 0;
    std::optional<int64_t> last_success_ms;
    std::string last_failure_reason;
};

// Static identity of a provider.
struct ProviderMetadata {
    std::string code;             // fred, dukascopy, ecb, chinamoney, ...
    std::string display_name;
    AccessMode access = AccessMode::keyless;
    std::string base_url;
    std::string terms_url;
    Capabilities capabilities;
    // Set when the provider needs a secret: the environment variable it is
    // read from. The value is never held here.
    std::optional<std::string> credential_env_var;
};

// Abort predicate honoured inside long fetches so shutdown is prompt.
using ShouldStop = std::function<bool()>;

// The one interface every source implements.
//
// A provider's job ends at "here is what the source published, normalised and
// timestamped". It does not decide whether a number is interesting, does not
// compare series, and computes nothing beyond unit and timezone normalisation.
class Provider {
public:
    virtual ~Provider() = default;

    [[nodiscard]] virtual const ProviderMetadata& metadata() const = 0;

    // Instruments this provider genuinely serves, read from its catalogue where
    // it publishes one and verified by probe where it does not.
    [[nodiscard]] virtual Result<std::vector<InstrumentDescriptor>>
    discover_instruments(const ShouldStop& should_stop) = 0;

    // Closed historical window. Providers that cannot serve bars return
    // invalid_argument rather than inventing them from a daily series.
    [[nodiscard]] virtual Result<BarPage>
    fetch_historical_bars(const InstrumentDescriptor& instrument, const std::string& timeframe,
                          Window window, const ShouldStop& should_stop) {
        (void)instrument; (void)timeframe; (void)window; (void)should_stop;
        return fail(ErrorCode::invalid_argument, metadata().code + " does not serve price bars");
    }

    [[nodiscard]] virtual Result<ReferencePage>
    fetch_historical_reference(const InstrumentDescriptor& instrument, Window window,
                               const ShouldStop& should_stop) {
        (void)instrument; (void)window; (void)should_stop;
        return fail(ErrorCode::invalid_argument, metadata().code + " does not serve reference values");
    }

    // As the source knew it on `vintage_date`. Only providers declaring
    // `capabilities.vintages` implement this; the rest say so plainly rather
    // than returning today's number under a past date.
    [[nodiscard]] virtual Result<ReleasePage>
    fetch_vintage(const InstrumentDescriptor& instrument, const std::string& vintage_date,
                  Window window, const ShouldStop& should_stop) {
        (void)instrument; (void)vintage_date; (void)window; (void)should_stop;
        return fail(ErrorCode::invalid_argument, metadata().code + " does not serve vintages");
    }

    // Most recent observation(s). Used by the live poller.
    [[nodiscard]] virtual Result<ReferencePage>
    fetch_latest(const InstrumentDescriptor& instrument, const ShouldStop& should_stop) = 0;

    // Cheap liveness probe. Must not count against a metered budget where the
    // provider has one.
    [[nodiscard]] virtual Result<Health> health(const ShouldStop& should_stop) = 0;
};

using ProviderPtr = std::shared_ptr<Provider>;

} // namespace altmacro::providers
