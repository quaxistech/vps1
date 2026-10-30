#include "altmacro/providers/fred.hpp"

#include "altmacro/core/logger.hpp"
#include "altmacro/core/time.hpp"

#include <charconv>
#include <chrono>
#include <sstream>

namespace altmacro::providers {
namespace {

// FRED documents 2 requests per second. Half that is used, because the whole
// working set is a few dozen series fetched once and then polled daily, so
// there is nothing to gain from crowding a published limit.
constexpr double kRequestsPerSecond = 1.0;
constexpr double kBurst = 2.0;

std::optional<int64_t> iso_date_to_ms(const std::string& d) {
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return std::nullopt;
    int year = 0, month = 0, day = 0;
    const auto parse = [&](size_t begin, size_t end, int& value) {
        const auto result = std::from_chars(d.data() + begin, d.data() + end, value);
        return result.ec == std::errc{} && result.ptr == d.data() + end;
    };
    if (!parse(0,4,year) || !parse(5,7,month) || !parse(8,10,day) || year < 1) return std::nullopt;
    const std::chrono::year_month_day date{std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) return std::nullopt;
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::sys_days{date}.time_since_epoch()).count();
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

} // namespace

Result<int64_t> FredProvider::date_only_vintage_bound(const std::string& date) {
    const auto midnight = iso_date_to_ms(date);
    if (!midnight) return fail(ErrorCode::parse_error, "invalid vintage date: " + date);
    // End of source's US Eastern calendar date, conservative in both EST/EDT.
    // It may lag the summer bound by one hour; it never claims midnight known.
    return *midnight + 29LL * 3'600'000;
}

FredProvider::FredProvider(bool vintage_capable)
    : vintage_capable_(vintage_capable),
      host_(vintage_capable ? "alfred.stlouisfed.org" : "fred.stlouisfed.org"),
      bucket_(kRequestsPerSecond, kBurst) {
    meta_.code = vintage_capable ? "alfred" : "fred";
    meta_.display_name = vintage_capable ? "ALFRED vintage archive (St. Louis Fed)"
                                         : "Federal Reserve Bank of St. Louis (FRED)";
    meta_.access = AccessMode::keyless;
    meta_.base_url = "https://" + host_;
    meta_.terms_url = "https://fred.stlouisfed.org/docs/api/terms_of_use.html";
    meta_.capabilities.historical = true;
    meta_.capabilities.latest = true;
    meta_.capabilities.intraday = false;   // daily at best; never presented otherwise
    meta_.capabilities.vintages = vintage_capable;
    meta_.capabilities.bid_ask = false;
    meta_.capabilities.timeframes = {"1d"};
    meta_.capabilities.rate_limit_per_min = 120;   // documented: 2 requests/second
}

void FredProvider::set_series(std::vector<std::string> series_ids) {
    series_ = std::move(series_ids);
}

Result<core::HttpResponse> FredProvider::fetch(const std::string& target,
                                               const ShouldStop& should_stop) {
    static thread_local std::map<std::string, std::unique_ptr<core::HttpsClient>> clients;
    auto& client = clients[host_];
    if (!client) {
        client = std::make_unique<core::HttpsClient>(core::HttpOptions{
            .host = host_, .port = "443",
            .connect_timeout_ms = 10000, .request_timeout_ms = 60000,
            });
    }

    constexpr int kMaxAttempts = 5;
    int64_t delay_ms = 2000;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (should_stop && should_stop()) {
            return fail(ErrorCode::shutdown_requested, "shutdown while fetching " + target);
        }
        if (!bucket_.acquire(should_stop)) {
            return fail(ErrorCode::shutdown_requested, "gave up waiting for rate-limit token");
        }
        auto res = client->get(target);
        if (!res) {
            if (attempt == kMaxAttempts) return std::unexpected(res.error());
            bucket_.penalise(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min<int64_t>(delay_ms * 2, 60000);
            continue;
        }
        if (res->status == 200) return res;
        if (res->status == 404) return fail(ErrorCode::not_found, "no such series: " + target);
        if (res->status == 429 || res->status >= 500) {
            // Honour Retry-After when the server sends one; it knows better
            // than our backoff curve does.
            int64_t penalty = delay_ms;
            if (auto it = res->headers.find("retry-after"); it != res->headers.end()) {
                const int64_t secs = std::atoll(it->second.c_str());
                if (secs > 0) penalty = secs * 1000;
            }
            bucket_.penalise(std::chrono::milliseconds(penalty));
            log::warn("provider_throttled",
                      {{"provider", meta_.code}, {"status", res->status},
                       {"attempt", attempt}, {"backoff_ms", penalty}});
            delay_ms = std::min<int64_t>(delay_ms * 2, 60000);
            if (attempt == kMaxAttempts) {
                return fail(ErrorCode::rate_limited, "throttled by " + meta_.code, true);
            }
            continue;
        }
        return fail(ErrorCode::network_error,
                    "unexpected status " + std::to_string(res->status) + " for " + target);
    }
    return fail(ErrorCode::internal_error, "unreachable");
}

Result<std::vector<FredProvider::CsvPoint>>
FredProvider::parse_csv(const std::string& body, std::string* out_value_column) {
    std::istringstream in(body);
    std::string line;
    if (!std::getline(in, line)) {
        return fail(ErrorCode::parse_error, "empty CSV body");
    }
    line = trim(line);
    const auto comma = line.find(',');
    if (comma == std::string::npos) {
        return fail(ErrorCode::parse_error, "CSV header has no value column: " + line);
    }
    // The value column is named after the series, and under ALFRED it carries
    // the vintage as a suffix. Reading it back is how a vintage response is
    // confirmed to be the vintage that was asked for.
    if (out_value_column) *out_value_column = line.substr(comma + 1);

    std::vector<CsvPoint> points;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        const auto sep = line.find(',');
        if (sep == std::string::npos) {
            return fail(ErrorCode::parse_error, "malformed CSV row: " + line);
        }
        CsvPoint p;
        p.date = line.substr(0, sep);
        const std::string raw = trim(line.substr(sep + 1));
        // "." is FRED's marker for "no observation on this date". It becomes an
        // explicit absence, never a number.
        if (raw != "." && !raw.empty()) {
            double v = 0.0;
            const auto* first = raw.data();
            const auto* last = raw.data() + raw.size();
            const auto [ptr, ec] = std::from_chars(first, last, v);
            if (ec != std::errc{} || ptr != last) {
                return fail(ErrorCode::parse_error,
                            "value '" + raw + "' on " + p.date + " is not a number");
            }
            p.value = v;
        }
        points.push_back(std::move(p));
    }
    return points;
}

Result<std::vector<InstrumentDescriptor>>
FredProvider::discover_instruments(const ShouldStop& should_stop) {
    std::vector<InstrumentDescriptor> found;
    for (const auto& id : series_) {
        if (should_stop && should_stop()) break;
        auto res = fetch("/graph/fredgraph.csv?id=" + id, should_stop);
        if (!res) {
            if (res.error().code == ErrorCode::not_found) {
                log::warn("series_absent_at_provider", {{"provider", meta_.code}, {"series", id}});
                continue;
            }
            return std::unexpected(res.error());
        }
        std::string column;
        auto points = parse_csv(res->body, &column);
        if (!points) return std::unexpected(points.error());
        if (points->empty()) {
            log::warn("series_empty_at_provider", {{"provider", meta_.code}, {"series", id}});
            continue;
        }

        InstrumentDescriptor d;
        d.native_symbol = id;
        d.display_name = id;
        d.series_class = SeriesClass::official_reference;
        d.native_frequency = "daily";
        d.timestamp_semantics = TimestampSemantics::observation_date;
        d.source_timezone = "America/New_York";
        d.price_basis = PriceBasis::not_applicable;
        // Recorded from what the source actually served, so a later claim about
        // coverage is evidence rather than intention.
        d.earliest_available_ms = iso_date_to_ms(points->front().date);
        found.push_back(std::move(d));
    }
    return found;
}

Result<ReferencePage>
FredProvider::fetch_historical_reference(const InstrumentDescriptor& instrument, Window window,
                                         const ShouldStop& should_stop) {
    auto res = fetch("/graph/fredgraph.csv?id=" + instrument.native_symbol, should_stop);
    if (!res) return std::unexpected(res.error());

    std::string column;
    auto points = parse_csv(res->body, &column);
    if (!points) return std::unexpected(points.error());

    ReferencePage page;
    for (const auto& p : *points) {
        const auto ms = iso_date_to_ms(p.date);
        if (!ms) {
            return fail(ErrorCode::parse_error, "unparseable observation date: " + p.date);
        }
        if (*ms < window.start_ms || *ms >= window.end_ms) continue;

        ReferenceValue v;
        v.event_time_ms = *ms;
        // FRED's CSV carries no release timestamp. Leaving this empty is the
        // honest answer: the consumer then falls back to ingest_time, which is
        // an upper bound on when we could have known, rather than a fiction.
        v.publication_time_ms = std::nullopt;
        if (p.value.has_value()) {
            v.value = *p.value;
            v.quality = domain::QualityStatus::valid;
        } else {
            v.quality = domain::QualityStatus::missing;
            v.quality_reason = "source published '.' for this date";
        }
        page.values.push_back(std::move(v));
    }
    return page;
}

Result<ReleasePage>
FredProvider::fetch_vintage(const InstrumentDescriptor& instrument, const std::string& vintage_date,
                            Window window, const ShouldStop& should_stop) {
    if (!vintage_capable_) {
        return fail(ErrorCode::invalid_argument, "fred host does not serve vintages; use alfred");
    }
    const auto publication_bound = date_only_vintage_bound(vintage_date);
    if (!publication_bound) return std::unexpected(publication_bound.error());
    auto res = fetch("/graph/alfredgraph.csv?id=" + instrument.native_symbol +
                         "&vintage_date=" + vintage_date,
                     should_stop);
    if (!res) return std::unexpected(res.error());

    std::string column;
    auto points = parse_csv(res->body, &column);
    if (!points) return std::unexpected(points.error());

    // ALFRED names the value column SERIES_YYYYMMDD. Checking it is how we know
    // the server honoured the vintage rather than quietly serving today's
    // numbers — which would put a future revision into a past observation and
    // hand a model information it could not have had.
    std::string expected = instrument.native_symbol + "_";
    for (char c : vintage_date) {
        if (c != '-') expected.push_back(c);
    }
    if (column != expected) {
        return fail(ErrorCode::validation_error,
                    "vintage not honoured: asked for " + expected + ", server returned " + column);
    }

    ReleasePage page;
    for (const auto& p : *points) {
        const auto ms = iso_date_to_ms(p.date);
        if (!ms) return fail(ErrorCode::parse_error, "unparseable observation date: " + p.date);
        if (*ms < window.start_ms || *ms >= window.end_ms) continue;

        ReleaseObservation o;
        o.observation_time_ms = *ms;
        o.vintage_date = vintage_date;
        // EX-16 (2026-10-30): a vintage date has no time of day. The series
        // "as the archive knew it on that date" includes a release made at
        // any hour of that US calendar day; its midnight would claim the
        // value known up to a day early. The bound is therefore the END of
        // that day in US Eastern time, in UTC: the date's midnight + 29 h
        // (24 h + the 5 h of EST, the later of the two offsets). Never earlier.
        o.publication_time_ms = *publication_bound;
        if (p.value.has_value()) {
            o.value = *p.value;
            o.quality = domain::QualityStatus::valid;
        } else {
            o.quality = domain::QualityStatus::missing;
            o.quality_reason = "not published as of this vintage";
        }
        page.observations.push_back(std::move(o));
    }
    return page;
}

Result<ReferencePage> FredProvider::fetch_latest(const InstrumentDescriptor& instrument,
                                                 const ShouldStop& should_stop) {
    // The whole series is one small file, so "latest" is the tail of it. There
    // is no cheaper endpoint and no reason to invent one.
    const int64_t now = timeutil::now_ms();
    Window w{now - 30LL * 86'400'000, now + 86'400'000};
    return fetch_historical_reference(instrument, w, should_stop);
}

Result<Health> FredProvider::health(const ShouldStop& should_stop) {
    Health h;
    // DGS10 has existed since 1962 and is not going away; it is the cheapest
    // honest proof that the host is answering with real content.
    auto res = fetch("/graph/fredgraph.csv?id=DGS10", should_stop);
    if (res && res->body.find("DGS10") != std::string::npos) {
        h.reachable = true;
        h.last_success_ms = timeutil::now_ms();
        return h;
    }
    h.reachable = false;
    h.last_failure_reason = res ? "unexpected body" : res.error().message;
    return h;
}

} // namespace altmacro::providers
