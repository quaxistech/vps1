#include "altmacro/providers/frankfurter.hpp"

#include "altmacro/core/logger.hpp"
#include "altmacro/core/time.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>

namespace altmacro::providers {
namespace {

constexpr double kRequestsPerSecond = 1.0;
constexpr double kBurst = 3.0;

std::optional<int64_t> iso_date_to_ms(const std::string& d) {
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return std::nullopt;
    std::tm tm{};
    tm.tm_year = std::atoi(d.substr(0, 4).c_str()) - 1900;
    tm.tm_mon = std::atoi(d.substr(5, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(d.substr(8, 2).c_str());
    const std::time_t t = timegm(&tm);
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;
    return static_cast<int64_t>(t) * 1000;
}

std::string iso_date(int64_t ms) {
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

} // namespace

FrankfurterProvider::FrankfurterProvider() : bucket_(kRequestsPerSecond, kBurst) {
    meta_.code = "frankfurter";
    meta_.display_name = "Frankfurter aggregated central-bank rates";
    meta_.access = AccessMode::keyless;
    meta_.base_url = "https://api.frankfurter.dev";
    meta_.terms_url = "https://frankfurter.dev";
    meta_.capabilities.historical = true;
    meta_.capabilities.latest = true;
    meta_.capabilities.intraday = false;
    meta_.capabilities.vintages = false;
    meta_.capabilities.bid_ask = false;
    meta_.capabilities.timeframes = {"1d"};
    meta_.capabilities.rate_limit_per_min = 60;   // ours: the provider publishes no number
}

void FrankfurterProvider::set_wanted_sources(std::vector<std::string> source_keys) {
    wanted_ = std::move(source_keys);
}

Result<FrankfurterProvider::Binding>
FrankfurterProvider::parse_symbol(const std::string& native_symbol) {
    const auto first = native_symbol.find(':');
    if (first == std::string::npos) {
        return fail(ErrorCode::config_error,
                    "frankfurter symbol '" + native_symbol +
                        "' must be PROVIDER:BASE:QUOTE; querying without an explicit provider "
                        "returns a blend across 97 sources and is nobody's published rate");
    }
    const auto second = native_symbol.find(':', first + 1);
    if (second == std::string::npos) {
        return fail(ErrorCode::config_error,
                    "frankfurter symbol '" + native_symbol + "' is missing its quote currency");
    }
    Binding b;
    b.source = native_symbol.substr(0, first);
    b.base = native_symbol.substr(first + 1, second - first - 1);
    b.quote = native_symbol.substr(second + 1);
    if (b.source.empty() || b.base.empty() || b.quote.empty()) {
        return fail(ErrorCode::config_error, "frankfurter symbol has an empty part");
    }
    return b;
}

Result<core::HttpResponse> FrankfurterProvider::fetch(const std::string& target,
                                                      const ShouldStop& should_stop) {
    static thread_local core::HttpsClient client(core::HttpOptions{
        .host = "api.frankfurter.dev", .port = "443",
        .connect_timeout_ms = 10000, .request_timeout_ms = 45000,
    });

    constexpr int kMaxAttempts = 5;
    int64_t delay_ms = 2000;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (should_stop && should_stop()) {
            return fail(ErrorCode::shutdown_requested, "shutdown while fetching " + target);
        }
        if (!bucket_.acquire(should_stop)) {
            return fail(ErrorCode::shutdown_requested, "gave up waiting for rate-limit token");
        }
        auto res = client.get(target);
        if (!res) {
            if (attempt == kMaxAttempts) return std::unexpected(res.error());
            bucket_.penalise(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min<int64_t>(delay_ms * 2, 60000);
            continue;
        }
        if (res->status == 200) return res;
        if (res->status == 404) return fail(ErrorCode::not_found, "no such resource: " + target);
        if (res->status == 422) {
            // The API names the offending parameter in the body, which is more
            // useful than a status alone.
            return fail(ErrorCode::invalid_argument,
                        "frankfurter rejected the request: " + res->body.substr(0, 200));
        }
        if (res->status == 429 || res->status >= 500) {
            int64_t penalty = delay_ms;
            if (auto it = res->headers.find("retry-after"); it != res->headers.end()) {
                const int64_t secs = std::atoll(it->second.c_str());
                if (secs > 0) penalty = secs * 1000;
            }
            bucket_.penalise(std::chrono::milliseconds(penalty));
            log::warn("provider_throttled", {{"provider", "frankfurter"},
                                             {"status", res->status}, {"backoff_ms", penalty}});
            delay_ms = std::min<int64_t>(delay_ms * 2, 60000);
            if (attempt == kMaxAttempts) {
                return fail(ErrorCode::rate_limited, "throttled by frankfurter", true);
            }
            continue;
        }
        return fail(ErrorCode::network_error,
                    "unexpected status " + std::to_string(res->status) + " for " + target);
    }
    return fail(ErrorCode::internal_error, "unreachable");
}

Result<std::vector<InstrumentDescriptor>>
FrankfurterProvider::discover_instruments(const ShouldStop& should_stop) {
    auto res = fetch("/v2/providers", should_stop);
    if (!res) return std::unexpected(res.error());

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(res->body);
    } catch (const std::exception& e) {
        return fail(ErrorCode::parse_error, std::string("provider catalogue is not JSON: ") + e.what());
    }
    if (!doc.is_array()) {
        return fail(ErrorCode::parse_error, "provider catalogue is not an array");
    }

    std::vector<InstrumentDescriptor> found;
    for (const auto& p : doc) {
        const std::string key = p.value("key", "");
        if (key.empty()) continue;
        if (!wanted_.empty() &&
            std::find(wanted_.begin(), wanted_.end(), key) == wanted_.end()) {
            continue;
        }
        const std::string pivot = p.value("pivot_currency", "");
        // rate_type is the authority's own description of what it publishes.
        // It is carried into the instrument's denomination so the distinction
        // between a central parity, a reference rate and a market close is
        // never lost between here and the database.
        const std::string rate_type = p.contains("rate_type") && !p["rate_type"].is_null()
                                          ? p["rate_type"].get<std::string>()
                                          : "unspecified";
        const std::string start = p.value("start_date", "");

        if (!p.contains("currencies") || !p["currencies"].is_array()) continue;
        for (const auto& cur : p["currencies"]) {
            const std::string quote = cur.get<std::string>();
            if (quote == pivot) continue;

            InstrumentDescriptor d;
            d.native_symbol = key + ":" + pivot + ":" + quote;
            d.display_name = key + " " + pivot + "/" + quote + " (" + rate_type + ")";
            d.series_class = SeriesClass::official_reference;
            d.denomination = quote + " per " + pivot;
            d.unit = "price_ratio";
            d.native_frequency = p.value("frequency", "daily");
            d.timestamp_semantics = TimestampSemantics::observation_date;
            d.price_basis = PriceBasis::fixing;
            d.earliest_available_ms = start.empty() ? std::nullopt : iso_date_to_ms(start);
            found.push_back(std::move(d));
        }
    }
    return found;
}

Result<ReferencePage>
FrankfurterProvider::fetch_historical_reference(const InstrumentDescriptor& instrument,
                                                Window window, const ShouldStop& should_stop) {
    auto binding = parse_symbol(instrument.native_symbol);
    if (!binding) return std::unexpected(binding.error());

    // `to` is inclusive at the source; the window's end is exclusive here.
    const std::string target = "/v2/providers/" + binding->source + "/rates?from=" +
                               iso_date(window.start_ms) + "&to=" +
                               iso_date(window.end_ms - 1) + "&base=" + binding->base +
                               "&quotes=" + binding->quote;

    auto res = fetch(target, should_stop);
    if (!res) return std::unexpected(res.error());

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(res->body);
    } catch (const std::exception& e) {
        return fail(ErrorCode::parse_error, std::string("rates response is not JSON: ") + e.what());
    }
    if (!doc.is_array()) {
        return fail(ErrorCode::parse_error, "rates response is not an array");
    }

    ReferencePage page;
    for (const auto& row : doc) {
        const std::string date = row.value("date", "");
        const auto ms = iso_date_to_ms(date);
        if (!ms) return fail(ErrorCode::parse_error, "unparseable date in rates response: " + date);

        // Guard against a silent semantic swap: if the API ever answered with a
        // different pair than was asked for, storing it would put one currency's
        // history under another's name.
        if (row.value("base", binding->base) != binding->base ||
            row.value("quote", binding->quote) != binding->quote) {
            return fail(ErrorCode::validation_error,
                        "response pair " + row.value("base", "?") + "/" + row.value("quote", "?") +
                            " does not match the requested " + binding->base + "/" + binding->quote);
        }

        ReferenceValue v;
        v.event_time_ms = *ms;
        v.publication_time_ms = std::nullopt;   // the API states no release time
        if (row.contains("rate") && row["rate"].is_number()) {
            v.value = row["rate"].get<double>();
            v.quality = domain::QualityStatus::valid;
        } else {
            v.quality = domain::QualityStatus::missing;
            v.quality_reason = "source returned no rate for this date";
        }
        page.values.push_back(std::move(v));
    }
    return page;
}

Result<ReferencePage> FrankfurterProvider::fetch_latest(const InstrumentDescriptor& instrument,
                                                        const ShouldStop& should_stop) {
    const int64_t now = timeutil::now_ms();
    Window w{now - 14LL * 86'400'000, now + 86'400'000};
    return fetch_historical_reference(instrument, w, should_stop);
}

Result<Health> FrankfurterProvider::health(const ShouldStop& should_stop) {
    Health h;
    auto res = fetch("/v2/providers", should_stop);
    if (res && res->body.size() > 100) {
        h.reachable = true;
        h.last_success_ms = timeutil::now_ms();
        return h;
    }
    h.reachable = false;
    h.last_failure_reason = res ? "catalogue unexpectedly small" : res.error().message;
    return h;
}

} // namespace altmacro::providers
