#pragma once

#include "altmacro/core/http.hpp"
#include "altmacro/core/rate_limiter.hpp"
#include "altmacro/providers/provider.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace altmacro::providers {

// Dukascopy Bank's public historical datafeed.
//
// This is the backend of Dukascopy's free Historical Data Export tool. It is
// not a documented API: Dukascopy documents the tool and the JForex library,
// but publishes nothing about this endpoint — no schema, no limits, no
// stability promise. Everything below was therefore established by fetching
// real files and decoding them, and is written down here so the next reader
// does not have to rediscover it:
//
//   URL      /datafeed/{SYMBOL}/{YYYY}/{MM}/{DD}/{HH}h_ticks.bi5      (ticks)
//            /datafeed/{SYMBOL}/{YYYY}/{MM}/{DD}/BID_candles_min_1.bi5 (bars)
//            /datafeed/{SYMBOL}/{YYYY}/{MM}/{DD}/ASK_candles_min_1.bi5
//   MONTH    ZERO-BASED. January is "00" and December is "11". Getting this
//            wrong silently returns a real file for the wrong month.
//   BODY     LZMA, alone-format (not .xz): a 13-byte header with no magic.
//   TICK     20 bytes, big-endian: uint32 ms since the hour, uint32 ask,
//            uint32 bid, float32 ask volume, float32 bid volume.
//   BAR      24 bytes, big-endian: uint32 seconds since midnight, then
//            int32 open, close, low, high — in THAT order, not OHLC — then
//            float32 volume. A day file holds exactly 1440 records.
//   PRICES   integers scaled by an instrument-specific factor, which the feed
//            does not carry. See `decode_scale` in macro_instrument_sources.
//   TIME     UTC throughout; a bar's timestamp is its OPEN.
//   EMPTY    a zero-length 200 means the period exists and holds nothing,
//            i.e. the market was shut. It is not an error and is stored as
//            MARKET_CLOSED.
//   404      the symbol is not in the catalogue. Verified against a control:
//            a deliberately nonexistent symbol answers 404 in exactly the same
//            way, so a 200 is positive evidence the instrument is real.
//   LIMITS   undocumented. Measured behaviour: a burst draws 503s and is then
//            followed by minutes of refused connections. Pacing is therefore
//            deliberately conservative and a 503 lengthens it further.
class DukascopyProvider final : public Provider {
public:
    DukascopyProvider();

    [[nodiscard]] const ProviderMetadata& metadata() const override { return meta_; }

    // Dukascopy publishes no machine-readable catalogue: the instrument list
    // endpoint answers 403. Discovery therefore probes the configured symbols
    // against a known-good trading hour and reports only those that answer
    // 200, using a nonexistent control symbol to prove that a 404 really means
    // "absent" on the day of the probe.
    [[nodiscard]] Result<std::vector<InstrumentDescriptor>>
    discover_instruments(const ShouldStop& should_stop) override;

    [[nodiscard]] Result<BarPage>
    fetch_historical_bars(const InstrumentDescriptor& instrument, const std::string& timeframe,
                          Window window, const ShouldStop& should_stop) override;

    [[nodiscard]] Result<ReferencePage>
    fetch_latest(const InstrumentDescriptor& instrument, const ShouldStop& should_stop) override;

    [[nodiscard]] Result<Health> health(const ShouldStop& should_stop) override;

    // Symbols this provider will probe, set from configuration.
    void set_candidate_symbols(std::vector<std::string> symbols);
    // Per-symbol decode scale and the band a decoded price must land in.
    void set_decode(const std::string& symbol, double scale, double sane_min, double sane_max);

    // Exposed for tests: the wire formats above, decoded in isolation.
    struct Tick {
        int64_t event_time_ms = 0;
        double bid = 0.0;
        double ask = 0.0;
        double bid_volume = 0.0;
        double ask_volume = 0.0;
    };
    [[nodiscard]] static Result<std::string> lzma_decode(const std::string& compressed);
    [[nodiscard]] static Result<std::vector<Tick>>
    decode_ticks(const std::string& raw, int64_t hour_start_ms, double scale);
    [[nodiscard]] static Result<std::vector<PriceBar>>
    decode_minute_bars(const std::string& raw, int64_t day_start_ms, double scale, PriceBasis basis);
    // Zero-based month, as the feed requires.
    [[nodiscard]] static std::string bar_path(const std::string& symbol, int64_t day_start_ms,
                                              PriceBasis basis);
    [[nodiscard]] static std::string tick_path(const std::string& symbol, int64_t hour_start_ms);

    // What a 404 means for a given day. A 404 says the archive has no file --
    // never that the market was shut. Measured on this feed: a genuine weekend
    // answers 200 with a full 1440-record file whose quiet minutes are
    // all-zero, while a day not yet archived answers 404. Filing the latter as
    // a closure would tell a consumer the FX market shut on a Thursday.
    [[nodiscard]] static AbsenceReason classify_missing_day(int64_t day_start_ms, int64_t now_ms);

    // A trading hour to probe a symbol against: the most recent Tuesday 10:00
    // UTC that is at least a week old.
    //
    // Derived rather than frozen. A fixed date works until it doesn't: it stays
    // where it is while the calendar moves, so an instrument listed after that
    // date would be probed on a day it did not exist and reported absent
    // forever. A week of margin keeps it clear of the archive's write lag, and
    // Tuesday is never a weekend or a Monday holiday on any calendar we touch.
    [[nodiscard]] static int64_t probe_hour_ms(int64_t now_ms);

    // Aggregate ticks into one-minute bars on one side of the book.
    //
    // The daily candle file does not exist until the day is over -- measured:
    // 404 for the current day, 200 with a full 1440 records afterwards -- but
    // the HOURLY tick files appear within hours. Without this the service
    // would be a day behind on every intraday series while the source had the
    // data all along.
    //
    // Only minutes that actually had ticks are emitted, and they are VALID.
    // The quiet minutes in between are not invented here: the daily file
    // supplies them later, labelled STALE, and the store refuses to let that
    // downgrade anything already recorded as traded.
    [[nodiscard]] static std::vector<PriceBar>
    bars_from_ticks(const std::vector<Tick>& ticks, PriceBasis basis);

private:
    ProviderMetadata meta_;
    struct Decode { double scale; double sane_min; double sane_max; };
    std::vector<std::string> candidates_;
    std::map<std::string, Decode> decode_;
    core::TokenBucket bucket_;

    [[nodiscard]] Result<core::HttpResponse>
    fetch(const std::string& target, const ShouldStop& should_stop);

    // Builds the current day's bars from its hourly tick files, which exist
    // before the daily candle file does.
    [[nodiscard]] Result<std::vector<PriceBar>>
    fetch_day_from_ticks(const std::string& symbol, int64_t day_start_ms, double scale,
                         PriceBasis basis, Window window, const ShouldStop& should_stop);
};

} // namespace altmacro::providers
