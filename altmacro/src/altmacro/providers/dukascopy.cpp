#include "altmacro/providers/dukascopy.hpp"

#include "altmacro/core/logger.hpp"
#include "altmacro/core/time.hpp"

#include <lzma.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <ctime>

namespace altmacro::providers {
namespace {

constexpr size_t kTickRecord = 20;
constexpr size_t kBarRecord = 24;
constexpr int64_t kMinuteMs = 60'000;
constexpr int64_t kHourMs = 3'600'000;
constexpr int64_t kDayMs = 86'400'000;

// The feed is big-endian; the host is not. Read explicitly rather than relying
// on a cast, so this stays correct if it is ever built somewhere else.
uint32_t be32(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return (static_cast<uint32_t>(u[0]) << 24) | (static_cast<uint32_t>(u[1]) << 16) |
           (static_cast<uint32_t>(u[2]) << 8) | static_cast<uint32_t>(u[3]);
}

int32_t be32s(const char* p) { return static_cast<int32_t>(be32(p)); }

float be_float(const char* p) { return std::bit_cast<float>(be32(p)); }

// Zero-based month, two digits, as the feed requires. January is "00".
std::string path_for(const std::string& symbol, int64_t day_start_ms, const std::string& leaf) {
    const std::time_t secs = static_cast<std::time_t>(day_start_ms / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[192];
    std::snprintf(buf, sizeof(buf), "/datafeed/%s/%04d/%02d/%02d/%s",
                  symbol.c_str(), tm.tm_year + 1900, tm.tm_mon, tm.tm_mday, leaf.c_str());
    return buf;
}

} // namespace

std::string DukascopyProvider::bar_path(const std::string& symbol, int64_t day_start_ms,
                                        PriceBasis basis) {
    const char* side = (basis == PriceBasis::ask) ? "ASK" : "BID";
    return path_for(symbol, day_start_ms, std::string(side) + "_candles_min_1.bi5");
}

AbsenceReason DukascopyProvider::classify_missing_day(int64_t day_start_ms, int64_t now_ms) {
    const int64_t today_start = (now_ms / kDayMs) * kDayMs;
    return day_start_ms >= today_start ? AbsenceReason::not_in_archive_yet
                                       : AbsenceReason::absent_at_source;
}

std::string DukascopyProvider::tick_path(const std::string& symbol, int64_t hour_start_ms) {
    const int64_t day = (hour_start_ms / kDayMs) * kDayMs;
    const int hour = static_cast<int>((hour_start_ms - day) / kHourMs);
    char leaf[32];
    std::snprintf(leaf, sizeof(leaf), "%02dh_ticks.bi5", hour);
    return path_for(symbol, day, leaf);
}

// The payload is LZMA "alone" format — the 13-byte legacy header, not .xz.
// lzma_stream_decoder() rejects it; lzma_alone_decoder() is the right one.
Result<std::string> DukascopyProvider::lzma_decode(const std::string& compressed) {
    if (compressed.empty()) return std::string{};

    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_alone_decoder(&strm, UINT64_MAX) != LZMA_OK) {
        return fail(ErrorCode::internal_error, "lzma_alone_decoder init failed");
    }

    std::string out;
    out.reserve(compressed.size() * 6);
    std::vector<uint8_t> buf(64 * 1024);

    strm.next_in = reinterpret_cast<const uint8_t*>(compressed.data());
    strm.avail_in = compressed.size();

    lzma_ret ret = LZMA_OK;
    do {
        strm.next_out = buf.data();
        strm.avail_out = buf.size();
        ret = lzma_code(&strm, LZMA_FINISH);
        if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
            lzma_end(&strm);
            return fail(ErrorCode::parse_error,
                        "lzma decode failed with code " + std::to_string(static_cast<int>(ret)));
        }
        out.append(reinterpret_cast<const char*>(buf.data()), buf.size() - strm.avail_out);
    } while (ret != LZMA_STREAM_END);

    lzma_end(&strm);
    return out;
}

Result<std::vector<DukascopyProvider::Tick>>
DukascopyProvider::decode_ticks(const std::string& raw, int64_t hour_start_ms, double scale) {
    if (raw.size() % kTickRecord != 0) {
        return fail(ErrorCode::parse_error,
                    "tick payload is " + std::to_string(raw.size()) +
                        " bytes, not a multiple of " + std::to_string(kTickRecord));
    }
    std::vector<Tick> ticks;
    ticks.reserve(raw.size() / kTickRecord);
    int64_t previous_ms = -1;
    for (size_t off = 0; off + kTickRecord <= raw.size(); off += kTickRecord) {
        const char* p = raw.data() + static_cast<ptrdiff_t>(off);
        Tick t;
        const uint32_t ms = be32(p);
        // A tick claiming to be outside its own hour means the file was
        // misfiled or the offset base is wrong; either way the timestamp
        // cannot be trusted, and a wrong timestamp is worse than no tick.
        if (ms >= static_cast<uint32_t>(kHourMs)) {
            return fail(ErrorCode::parse_error,
                        "tick offset " + std::to_string(ms) + " ms lies outside its hour");
        }
        t.event_time_ms = hour_start_ms + static_cast<int64_t>(ms);
        t.ask = static_cast<double>(be32(p + 4)) * scale;
        t.bid = static_cast<double>(be32(p + 8)) * scale;
        t.ask_volume = static_cast<double>(be_float(p + 12));
        t.bid_volume = static_cast<double>(be_float(p + 16));
        // Ticks arrive in order; if they ever did not, storing them as-is
        // would produce a series that is silently out of sequence.
        if (t.event_time_ms < previous_ms) {
            return fail(ErrorCode::parse_error, "ticks are not in ascending time order");
        }
        previous_ms = t.event_time_ms;
        ticks.push_back(t);
    }
    return ticks;
}

Result<std::vector<PriceBar>>
DukascopyProvider::decode_minute_bars(const std::string& raw, int64_t day_start_ms, double scale,
                                      PriceBasis basis) {
    if (raw.size() % kBarRecord != 0) {
        return fail(ErrorCode::parse_error,
                    "bar payload is " + std::to_string(raw.size()) +
                        " bytes, not a multiple of " + std::to_string(kBarRecord));
    }
    std::vector<PriceBar> bars;
    bars.reserve(raw.size() / kBarRecord);
    for (size_t off = 0; off + kBarRecord <= raw.size(); off += kBarRecord) {
        const char* p = raw.data() + static_cast<ptrdiff_t>(off);
        const uint32_t sec = be32(p);
        if (sec >= static_cast<uint32_t>(kDayMs / 1000)) {
            return fail(ErrorCode::parse_error,
                        "bar offset " + std::to_string(sec) + " s lies outside its day");
        }
        // Field order on the wire is open, CLOSE, low, high — not OHLC.
        // Reading it as OHLC produces bars that pass every eyeball test and
        // are wrong on every candle that closed below its open.
        const int32_t o = be32s(p + 4);
        const int32_t c = be32s(p + 8);
        const int32_t l = be32s(p + 12);
        const int32_t h = be32s(p + 16);
        const float v = be_float(p + 20);

        PriceBar bar;
        bar.timeframe = "1m";
        bar.event_time_ms = day_start_ms + static_cast<int64_t>(sec) * 1000;
        bar.basis = basis;

        // An all-zero record is an absence, and the one thing this service must
        // never do is store an absence as the number zero.
        if (o == 0 && c == 0 && l == 0 && h == 0) {
            bar.quality = domain::QualityStatus::market_closed;
            bar.quality_reason = "no quotes in this minute at source";
            bars.push_back(bar);
            continue;
        }

        bar.open = static_cast<double>(o) * scale;
        bar.close = static_cast<double>(c) * scale;
        bar.low = static_cast<double>(l) * scale;
        bar.high = static_cast<double>(h) * scale;
        bar.volume = static_cast<double>(v);

        // Zero tick volume means no tick arrived in this minute, and the feed
        // does NOT write zeros for such minutes -- it repeats the last price.
        // A weekend therefore arrives as 1440 perfectly well-formed bars at a
        // frozen price, which is how 29% of this archive would otherwise be
        // stored as traded observations: a market that dealt flat, all weekend,
        // at no volume.
        //
        // Verified against the whole loaded window before this rule was
        // written: of 2.9 million bars, every single zero-volume bar is flat
        // (open == high == low == close) and there is not one counter-example,
        // while minutes that genuinely traded once are flat AND carry volume,
        // so they keep their VALID status.
        //
        // The price is kept rather than discarded -- it is the last known one
        // and is genuinely useful for an as-of join -- but it is labelled STALE so that
        // nothing downstream can mistake it for something that traded.
        const bool flat = (o == c && c == l && l == h);
        if (v == 0.0f && flat) {
            bar.quality = domain::QualityStatus::stale;
            bar.quality_reason = "no ticks at source in this minute; the feed carries the "
                                 "previous price forward";
        } else {
            bar.quality = domain::QualityStatus::valid;
        }

        if (!(*bar.high >= *bar.open && *bar.high >= *bar.close && *bar.high >= *bar.low &&
              *bar.low <= *bar.open && *bar.low <= *bar.close)) {
            return fail(ErrorCode::parse_error,
                        "decoded bar violates OHLC ordering; field layout or scale is wrong");
        }
        bars.push_back(bar);
    }
    return bars;
}

} // namespace altmacro::providers

// ---------------------------------------------------------------------------
// Network side
// ---------------------------------------------------------------------------

namespace altmacro::providers {
namespace {

// Deliberately slow. Dukascopy publishes no limit for this endpoint, and the
// measured behaviour is that a burst earns 503s followed by minutes of refused
// connections. One request every ten seconds with no burst allowance keeps a
// 283-day backfill inside a few hours and has never been throttled in testing.
constexpr double kRequestsPerSecond = 0.1;
constexpr double kBurst = 1.0;

// A symbol that cannot exist. Used as a control so a 404 can be read as
// "absent from the catalogue" rather than "the server is unhappy with us".
constexpr const char* kControlSymbol = "ZZZQQQ";

} // namespace

std::vector<PriceBar> DukascopyProvider::bars_from_ticks(const std::vector<Tick>& ticks,
                                                         PriceBasis basis) {
    std::vector<PriceBar> bars;
    if (ticks.empty()) return bars;

    const bool want_ask = (basis == PriceBasis::ask);
    int64_t current_minute = -1;
    PriceBar bar;

    for (const auto& t : ticks) {
        const int64_t minute = (t.event_time_ms / kMinuteMs) * kMinuteMs;
        const double price = want_ask ? t.ask : t.bid;
        const double volume = want_ask ? t.ask_volume : t.bid_volume;

        if (minute != current_minute) {
            if (current_minute >= 0) bars.push_back(bar);
            bar = PriceBar{};
            bar.timeframe = "1m";
            bar.event_time_ms = minute;
            bar.basis = basis;
            bar.quality = domain::QualityStatus::valid;
            bar.open = price;
            bar.high = price;
            bar.low = price;
            bar.volume = 0.0;
            current_minute = minute;
        }
        bar.high = std::max(*bar.high, price);
        bar.low = std::min(*bar.low, price);
        bar.close = price;
        bar.volume = *bar.volume + volume;
    }
    bars.push_back(bar);
    return bars;
}

int64_t DukascopyProvider::probe_hour_ms(int64_t now_ms) {
    // Step back a week, then to the Tuesday at or before that, then to 10:00 UTC.
    int64_t day = ((now_ms - 7 * kDayMs) / kDayMs) * kDayMs;
    const std::time_t secs = static_cast<std::time_t>(day / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    const int back_to_tuesday = (tm.tm_wday - 2 + 7) % 7;   // tm_wday: 0 = Sunday
    day -= static_cast<int64_t>(back_to_tuesday) * kDayMs;
    return day + 10 * kHourMs;
}

DukascopyProvider::DukascopyProvider() : bucket_(kRequestsPerSecond, kBurst) {
    meta_.code = "dukascopy";
    meta_.display_name = "Dukascopy Bank historical datafeed";
    meta_.access = AccessMode::keyless;
    meta_.base_url = "https://datafeed.dukascopy.com";
    meta_.terms_url = "https://www.dukascopy.com/swiss/english/marketwatch/historical/";
    meta_.capabilities.historical = true;
    meta_.capabilities.latest = false;   // the feed lags; the poller reads recent days, not a tick
    meta_.capabilities.intraday = true;
    meta_.capabilities.vintages = false;
    meta_.capabilities.bid_ask = true;
    meta_.capabilities.timeframes = {"1m"};
    // Recorded as the pacing we impose, not as a limit the provider published.
    meta_.capabilities.rate_limit_per_min = 6;
}

void DukascopyProvider::set_candidate_symbols(std::vector<std::string> symbols) {
    candidates_ = std::move(symbols);
}

void DukascopyProvider::set_decode(const std::string& symbol, double scale, double sane_min,
                                   double sane_max) {
    decode_[symbol] = Decode{scale, sane_min, sane_max};
}

Result<core::HttpResponse> DukascopyProvider::fetch(const std::string& target,
                                                    const ShouldStop& should_stop) {
    static thread_local core::HttpsClient client(core::HttpOptions{
        .host = "datafeed.dukascopy.com",
        .port = "443",
        .connect_timeout_ms = 10000,
        .request_timeout_ms = 60000,
    });

    constexpr int kMaxAttempts = 6;
    int64_t delay_ms = 5000;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (should_stop && should_stop()) {
            return fail(ErrorCode::shutdown_requested, "shutdown while fetching " + target);
        }
        if (!bucket_.acquire(should_stop)) {
            return fail(ErrorCode::shutdown_requested, "gave up waiting for rate-limit token");
        }

        auto res = client.get(target);
        if (!res) {
            // A refused connection is what throttling looks like from here, so
            // it penalises the bucket exactly as an explicit 503 would.
            bucket_.penalise(std::chrono::milliseconds(delay_ms));
            log::warn("provider_transport_error",
                      {{"provider", "dukascopy"}, {"target", target},
                       {"attempt", attempt}, {"backoff_ms", delay_ms},
                       {"error", res.error().message}});
            delay_ms = std::min<int64_t>(delay_ms * 2, 240000);
            if (attempt == kMaxAttempts) return std::unexpected(res.error());
            continue;
        }

        if (res->status == 200) return res;
        if (res->status == 404) {
            return fail(ErrorCode::not_found, "not in catalogue: " + target);
        }
        if (res->status == 429 || res->status >= 500) {
            bucket_.penalise(std::chrono::milliseconds(delay_ms));
            log::warn("provider_throttled",
                      {{"provider", "dukascopy"}, {"target", target},
                       {"status", res->status}, {"attempt", attempt}, {"backoff_ms", delay_ms}});
            delay_ms = std::min<int64_t>(delay_ms * 2, 240000);
            if (attempt == kMaxAttempts) {
                return fail(ErrorCode::rate_limited,
                            "throttled after " + std::to_string(kMaxAttempts) + " attempts", true);
            }
            continue;
        }
        return fail(ErrorCode::network_error,
                    "unexpected status " + std::to_string(res->status) + " for " + target);
    }
    return fail(ErrorCode::internal_error, "unreachable");
}

Result<std::vector<PriceBar>>
DukascopyProvider::fetch_day_from_ticks(const std::string& symbol, int64_t day_start_ms,
                                        double scale, PriceBasis basis, Window window,
                                        const ShouldStop& should_stop) {
    std::vector<PriceBar> out;
    const int64_t now = timeutil::now_ms();
    // Only hours that have already finished: the file for the hour in progress
    // is incomplete and would produce a partial final bar that the next pass
    // would have to correct.
    const int64_t last_complete_hour = ((now / kHourMs) * kHourMs) - kHourMs;

    for (int64_t hour = day_start_ms; hour <= last_complete_hour && hour < window.end_ms;
         hour += kHourMs) {
        if (hour + kHourMs <= window.start_ms) continue;
        if (should_stop && should_stop()) break;

        auto res = fetch(tick_path(symbol, hour), should_stop);
        if (!res) {
            // An hour the archive has not reached yet ends the walk; anything
            // else is a real failure and is reported rather than swallowed.
            if (res.error().code == ErrorCode::not_found) break;
            return std::unexpected(res.error());
        }
        if (res->body.empty()) continue;   // the hour exists and holds no ticks

        auto raw = lzma_decode(res->body);
        if (!raw) return std::unexpected(raw.error());
        auto ticks = decode_ticks(*raw, hour, scale);
        if (!ticks) return std::unexpected(ticks.error());

        auto bars = bars_from_ticks(*ticks, basis);
        for (auto& b : bars) {
            if (b.event_time_ms >= window.start_ms && b.event_time_ms < window.end_ms) {
                out.push_back(std::move(b));
            }
        }
    }
    return out;
}

Result<std::vector<InstrumentDescriptor>>
DukascopyProvider::discover_instruments(const ShouldStop& should_stop) {
    // The catalogue endpoint answers 403, so there is nothing to enumerate.
    // Instead each configured symbol is probed, and the control symbol proves
    // on this run that a 404 still means "absent" rather than "blocked".
    auto control = fetch(tick_path(kControlSymbol, probe_hour_ms(timeutil::now_ms())), should_stop);
    if (control.has_value()) {
        return fail(ErrorCode::validation_error,
                    "control symbol " + std::string(kControlSymbol) +
                        " returned data; a 404 can no longer be read as 'absent' and discovery "
                        "would report every symbol as present");
    }
    if (control.error().code != ErrorCode::not_found) {
        return fail(control.error().code,
                    "control probe did not reach the server cleanly: " + control.error().message,
                    true);
    }

    std::vector<InstrumentDescriptor> found;
    for (const auto& symbol : candidates_) {
        if (should_stop && should_stop()) break;

        const auto it = decode_.find(symbol);
        if (it == decode_.end()) {
            // Without a verified scale the prices cannot be decoded, and a
            // guessed scale is silently wrong by a factor of ten. The symbol is
            // reported unavailable rather than approximated.
            log::warn("instrument_rejected_no_decode_scale",
                      {{"provider", "dukascopy"}, {"symbol", symbol},
                       {"reason", "no verified decode scale configured"}});
            continue;
        }

        auto probe = fetch(tick_path(symbol, probe_hour_ms(timeutil::now_ms())), should_stop);
        if (!probe) {
            if (probe.error().code == ErrorCode::not_found) {
                log::info("instrument_absent_at_provider",
                          {{"provider", "dukascopy"}, {"symbol", symbol}});
                continue;
            }
            return std::unexpected(probe.error());
        }

        InstrumentDescriptor d;
        d.native_symbol = symbol;
        d.display_name = symbol;
        d.series_class = SeriesClass::market;
        d.native_frequency = "tick";
        d.timestamp_semantics = TimestampSemantics::bar_open;
        d.source_timezone = "UTC";
        d.session_calendar = "fx_24x5";
        d.price_basis = PriceBasis::bid;
        found.push_back(std::move(d));
    }
    return found;
}

Result<BarPage>
DukascopyProvider::fetch_historical_bars(const InstrumentDescriptor& instrument,
                                         const std::string& timeframe, Window window,
                                         const ShouldStop& should_stop) {
    if (timeframe != "1m") {
        return fail(ErrorCode::invalid_argument,
                    "dukascopy serves 1m bars here; " + timeframe +
                        " would have to be aggregated, which this provider does not do");
    }
    const auto it = decode_.find(instrument.native_symbol);
    if (it == decode_.end()) {
        return fail(ErrorCode::config_error,
                    "no verified decode scale for " + instrument.native_symbol +
                        "; refusing to guess one");
    }
    const double scale = it->second.scale;

    // One request serves one calendar day, so the window is walked a day at a
    // time and the caller's cursor advances with it. Nothing is held beyond the
    // day being decoded.
    constexpr int64_t kDay = 86'400'000;
    const int64_t day_start = (window.start_ms / kDay) * kDay;
    if (day_start >= window.end_ms) {
        return BarPage{};
    }

    const PriceBasis basis =
        (instrument.price_basis == PriceBasis::ask) ? PriceBasis::ask : PriceBasis::bid;

    auto res = fetch(bar_path(instrument.native_symbol, day_start, basis), should_stop);
    BarPage page;
    const int64_t next_day = day_start + kDay;
    page.next_cursor_ms = (next_day < window.end_ms) ? std::optional<int64_t>(next_day)
                                                     : std::nullopt;

    if (!res) {
        if (res.error().code == ErrorCode::not_found) {
            // A 404 says the archive has no file for this day. It says NOTHING
            // about whether the market traded, and the two must not be
            // conflated: measured on this feed, a genuine weekend answers 200
            // with a full 1440-record file whose quiet minutes are all-zero,
            // while a day that has not been archived yet -- today, or anything
            // future -- answers 404. Recording the latter as a closed market
            // would tell a consumer the FX market shut on a Thursday.
            const auto why = classify_missing_day(day_start, timeutil::now_ms());
            if (why == AbsenceReason::not_in_archive_yet) {
                // The daily file is not written until the day is over, but the
                // hourly TICK files appear within hours. Falling back to them
                // is the difference between a service that is a day behind and
                // one that is an hour behind, on data the source already has.
                auto live = fetch_day_from_ticks(instrument.native_symbol, day_start, scale,
                                                 basis, window, should_stop);
                if (!live) return std::unexpected(live.error());
                if (!live->empty()) {
                    page.bars = std::move(*live);
                    page.absence = AbsenceReason::none;
                    return page;
                }
            }
            page.absence = why;
            return page;
        }
        return std::unexpected(res.error());
    }

    if (res->body.empty()) {
        // A zero-length 200: the file exists and holds nothing. This is the
        // only response that genuinely means the period was shut.
        page.absence = AbsenceReason::market_closed;
        return page;
    }

    auto raw = lzma_decode(res->body);
    if (!raw) return std::unexpected(raw.error());

    auto bars = decode_minute_bars(*raw, day_start, scale, basis);
    if (!bars) return std::unexpected(bars.error());

    // Trim to the requested window; the file always covers a whole day.
    for (auto& b : *bars) {
        if (b.event_time_ms >= window.start_ms && b.event_time_ms < window.end_ms) {
            page.bars.push_back(std::move(b));
        }
    }
    return page;
}

Result<ReferencePage> DukascopyProvider::fetch_latest(const InstrumentDescriptor& instrument,
                                                      const ShouldStop& should_stop) {
    (void)instrument; (void)should_stop;
    // The archive is written after the fact. Presenting its most recent bar as
    // a live quote would promise a latency the source does not offer, so the
    // poller reads recent days through fetch_historical_bars instead.
    return fail(ErrorCode::invalid_argument,
                "dukascopy is an archive, not a live feed; use fetch_historical_bars");
}

Result<Health> DukascopyProvider::health(const ShouldStop& should_stop) {
    Health h;
    auto probe = fetch(tick_path(kControlSymbol, probe_hour_ms(timeutil::now_ms())), should_stop);
    // The control symbol must answer 404. Anything else — including success —
    // means the server is not behaving as the decoder assumes.
    if (!probe && probe.error().code == ErrorCode::not_found) {
        h.reachable = true;
        h.last_success_ms = timeutil::now_ms();
        return h;
    }
    h.reachable = false;
    h.last_failure_reason = probe.has_value() ? "control symbol returned data"
                                              : probe.error().message;
    return h;
}

} // namespace altmacro::providers
