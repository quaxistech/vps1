// Regression tests for the Dukascopy wire format.
//
// None of this is documented by Dukascopy. It was established by decoding real
// files, and these tests exist so that it cannot drift back into a guess: every
// property asserted here was verified against live data on 2026-09-10, and a
// change in any of them would silently corrupt an entire history.

#include "altmacro/providers/dukascopy.hpp"

#include <catch2/catch_test_macros.hpp>
#include <lzma.h>

#include <cstring>
#include <vector>

using altmacro::domain::PriceBasis;
using altmacro::domain::QualityStatus;
using altmacro::providers::DukascopyProvider;

namespace {

void put_be32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

void put_be_float(std::string& out, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put_be32(out, bits);
}

// One bar as the feed writes it: seconds, open, CLOSE, low, high, volume.
std::string bar_record(uint32_t sec, int32_t open, int32_t close, int32_t low, int32_t high,
                       float volume) {
    std::string r;
    put_be32(r, sec);
    put_be32(r, static_cast<uint32_t>(open));
    put_be32(r, static_cast<uint32_t>(close));
    put_be32(r, static_cast<uint32_t>(low));
    put_be32(r, static_cast<uint32_t>(high));
    put_be_float(r, volume);
    return r;
}

std::string tick_record(uint32_t ms, uint32_t ask, uint32_t bid, float av, float bv) {
    std::string r;
    put_be32(r, ms);
    put_be32(r, ask);
    put_be32(r, bid);
    put_be_float(r, av);
    put_be_float(r, bv);
    return r;
}

// Compress with the legacy alone container, which is what the feed serves.
std::string lzma_alone_compress(const std::string& in) {
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_options_lzma opts;
    REQUIRE(lzma_lzma_preset(&opts, LZMA_PRESET_DEFAULT) == 0);
    REQUIRE(lzma_alone_encoder(&strm, &opts) == LZMA_OK);

    std::string out;
    std::vector<uint8_t> buf(64 * 1024);
    strm.next_in = reinterpret_cast<const uint8_t*>(in.data());
    strm.avail_in = in.size();
    lzma_ret ret;
    do {
        strm.next_out = buf.data();
        strm.avail_out = buf.size();
        ret = lzma_code(&strm, LZMA_FINISH);
        REQUIRE((ret == LZMA_OK || ret == LZMA_STREAM_END));
        out.append(reinterpret_cast<const char*>(buf.data()), buf.size() - strm.avail_out);
    } while (ret != LZMA_STREAM_END);
    lzma_end(&strm);
    return out;
}

constexpr int64_t kDay = 1788825600000LL;   // 2026-09-08T00:00:00Z
constexpr int64_t kHour = 1788861600000LL;  // 2026-09-08T10:00:00Z

} // namespace

TEST_CASE("lzma alone format round-trips", "[dukascopy][lzma]") {
    // The feed uses the legacy .lzma container, not .xz. Decoding it with the
    // xz stream decoder fails outright, which is why the alone decoder is
    // named explicitly in the implementation.
    const std::string payload = bar_record(0, 116278, 116282, 116269, 116285, 212.79f);
    const auto compressed = lzma_alone_compress(payload);
    REQUIRE(compressed != payload);

    auto decoded = DukascopyProvider::lzma_decode(compressed);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == payload);
}

TEST_CASE("an empty body decodes to nothing rather than failing", "[dukascopy][lzma]") {
    // A zero-length 200 is how the feed says "this period is shut". It must not
    // be treated as a corrupt payload.
    auto decoded = DukascopyProvider::lzma_decode("");
    REQUIRE(decoded.has_value());
    CHECK(decoded->empty());
}

TEST_CASE("garbage is rejected, not silently accepted", "[dukascopy][lzma]") {
    auto decoded = DukascopyProvider::lzma_decode(std::string(64, '\x7f'));
    CHECK_FALSE(decoded.has_value());
}

TEST_CASE("bar field order is open, close, low, high", "[dukascopy][bars]") {
    // This is the property most likely to be got wrong and least likely to be
    // noticed: reading the record as OHLC yields bars that look fine and are
    // wrong on every candle that closed below its open. Verified against a real
    // day: 1440 bars decoded with 0 ordering violations under OCLH and 1179
    // violations under OHLC.
    std::string raw;
    // A down-bar: close is below open, and the true high is above both.
    raw += bar_record(0, 116300, 116250, 116240, 116310, 100.0f);
    auto bars = DukascopyProvider::decode_minute_bars(raw, kDay, 1e-5, PriceBasis::bid);
    REQUIRE(bars.has_value());
    REQUIRE(bars->size() == 1);

    const auto& b = bars->front();
    CHECK(b.open.value() == 1.163);
    CHECK(b.close.value() == 1.1625);
    CHECK(b.low.value() == 1.1624);
    CHECK(b.high.value() == 1.1631);
    CHECK(b.close.value() < b.open.value());
    CHECK(b.high.value() >= b.open.value());
    CHECK(b.low.value() <= b.close.value());
}

TEST_CASE("a record whose fields cannot be OHLC is refused", "[dukascopy][bars]") {
    // If the layout ever changed, the decoder must fail loudly rather than
    // store an impossible bar. Here "high" is below "low".
    std::string raw;
    raw += bar_record(0, 116300, 116250, 116400, 116100, 1.0f);
    auto bars = DukascopyProvider::decode_minute_bars(raw, kDay, 1e-5, PriceBasis::bid);
    CHECK_FALSE(bars.has_value());
}

TEST_CASE("an all-zero minute is an absence, never the number zero", "[dukascopy][bars]") {
    std::string raw;
    raw += bar_record(60, 0, 0, 0, 0, 0.0f);
    auto bars = DukascopyProvider::decode_minute_bars(raw, kDay, 1e-5, PriceBasis::bid);
    REQUIRE(bars.has_value());
    REQUIRE(bars->size() == 1);
    const auto& b = bars->front();
    CHECK(b.quality == QualityStatus::market_closed);
    CHECK_FALSE(b.open.has_value());
    CHECK_FALSE(b.close.has_value());
}

TEST_CASE("the decode scale is applied per instrument", "[dukascopy][scale]") {
    // The feed carries no scale. USDCNH and EURUSD are 1e-5; XAUUSD and USDJPY
    // are 1e-3. Using one where the other belongs produces a plausible series
    // that is wrong by a factor of a hundred.
    std::string raw;
    raw += bar_record(0, 4402705, 4403275, 4402000, 4404000, 5.0f);
    auto gold = DukascopyProvider::decode_minute_bars(raw, kDay, 1e-3, PriceBasis::bid);
    REQUIRE(gold.has_value());
    CHECK(gold->front().open.value() == 4402.705);

    auto wrong = DukascopyProvider::decode_minute_bars(raw, kDay, 1e-5, PriceBasis::bid);
    REQUIRE(wrong.has_value());
    // Internally consistent and externally absurd — which is precisely why the
    // sanity band exists downstream of this function.
    CHECK(wrong->front().open.value() == 44.02705);
}

TEST_CASE("bar timestamps are the bar's open, in UTC", "[dukascopy][time]") {
    std::string raw;
    raw += bar_record(0, 1, 1, 1, 1, 0.0f);
    raw += bar_record(60, 2, 2, 2, 2, 0.0f);
    raw += bar_record(86340, 3, 3, 3, 3, 0.0f);
    auto bars = DukascopyProvider::decode_minute_bars(raw, kDay, 1.0, PriceBasis::bid);
    REQUIRE(bars.has_value());
    REQUIRE(bars->size() == 3);
    CHECK((*bars)[0].event_time_ms == kDay);
    CHECK((*bars)[1].event_time_ms == kDay + 60'000);
    CHECK((*bars)[2].event_time_ms == kDay + 86'340'000);
}

TEST_CASE("a truncated file is refused", "[dukascopy][malformed]") {
    std::string raw = bar_record(0, 1, 1, 1, 1, 0.0f);
    raw.resize(raw.size() - 3);   // a partial trailing record
    auto bars = DukascopyProvider::decode_minute_bars(raw, kDay, 1.0, PriceBasis::bid);
    CHECK_FALSE(bars.has_value());
}

TEST_CASE("a record claiming to be outside its own day is refused", "[dukascopy][malformed]") {
    std::string raw = bar_record(86400, 1, 1, 1, 1, 0.0f);   // one second past midnight
    auto bars = DukascopyProvider::decode_minute_bars(raw, kDay, 1.0, PriceBasis::bid);
    CHECK_FALSE(bars.has_value());
}

TEST_CASE("ticks decode with both sides and their volumes", "[dukascopy][ticks]") {
    std::string raw;
    raw += tick_record(128, 670978, 670964, 2.69f, 0.90f);
    raw += tick_record(992, 670975, 670961, 0.90f, 0.90f);
    auto ticks = DukascopyProvider::decode_ticks(raw, kHour, 1e-5);
    REQUIRE(ticks.has_value());
    REQUIRE(ticks->size() == 2);

    const auto& t = ticks->front();
    CHECK(t.event_time_ms == kHour + 128);
    CHECK(t.bid == 6.70964);
    CHECK(t.ask == 6.70978);
    CHECK(t.bid < t.ask);
    CHECK(t.ask_volume > 0.0);
}

TEST_CASE("ticks going backwards in time are refused", "[dukascopy][ticks]") {
    std::string raw;
    raw += tick_record(5000, 100, 99, 1.0f, 1.0f);
    raw += tick_record(1000, 100, 99, 1.0f, 1.0f);
    auto ticks = DukascopyProvider::decode_ticks(raw, kHour, 1e-5);
    CHECK_FALSE(ticks.has_value());
}

TEST_CASE("a tick outside its hour is refused", "[dukascopy][ticks]") {
    std::string raw = tick_record(3'600'000, 100, 99, 1.0f, 1.0f);
    auto ticks = DukascopyProvider::decode_ticks(raw, kHour, 1e-5);
    CHECK_FALSE(ticks.has_value());
}

TEST_CASE("a partial tick record is refused", "[dukascopy][ticks]") {
    std::string raw = tick_record(0, 100, 99, 1.0f, 1.0f);
    raw.resize(raw.size() - 1);
    auto ticks = DukascopyProvider::decode_ticks(raw, kHour, 1e-5);
    CHECK_FALSE(ticks.has_value());
}

TEST_CASE("a 404 is never read as a closed market", "[dukascopy][absence]") {
    using altmacro::providers::AbsenceReason;
    // The defect: the first bootstrap filed two Thursdays as MARKET_CLOSED
    // because Dukascopy had not yet written that day's file when the walk
    // reached it. Measured on the live feed, a real weekend answers 200 with a
    // full 1440-record file -- a 404 only ever means "not in the archive".
    const int64_t thursday = 1789084800000LL;              // 2026-09-11T00:00:00Z
    const int64_t midday_same_day = thursday + 43'200'000;
    CHECK(DukascopyProvider::classify_missing_day(thursday, midday_same_day) ==
          AbsenceReason::not_in_archive_yet);

    // A future day is likewise not yet written, not shut.
    const int64_t next_week = thursday + 7 * 86'400'000;
    CHECK(DukascopyProvider::classify_missing_day(next_week, midday_same_day) ==
          AbsenceReason::not_in_archive_yet);

    // A past day the archive genuinely lacks is a coverage fact worth
    // investigating, and is classified as such rather than dismissed.
    const int64_t last_month = thursday - 30LL * 86'400'000;
    CHECK(DukascopyProvider::classify_missing_day(last_month, midday_same_day) ==
          AbsenceReason::absent_at_source);

    // The boundary: the first instant of today is still today.
    CHECK(DukascopyProvider::classify_missing_day(thursday, thursday) ==
          AbsenceReason::not_in_archive_yet);
    CHECK(DukascopyProvider::classify_missing_day(thursday - 86'400'000, thursday) ==
          AbsenceReason::absent_at_source);
}

TEST_CASE("a carried-forward price is not an observation", "[dukascopy][stale]") {
    // The defect this records: Dukascopy does NOT write zeros for minutes with
    // no ticks -- it repeats the last price at zero volume. A weekend therefore
    // arrives as 1440 immaculate bars at a frozen price, and 29% of the loaded
    // archive was stored as traded observations of a market that dealt flat,
    // all weekend, at no volume.
    std::string raw;
    raw += bar_record(0, 116113, 116113, 116113, 116113, 0.0f);
    auto bars = DukascopyProvider::decode_minute_bars(raw, kDay, 1e-5, PriceBasis::bid);
    REQUIRE(bars.has_value());
    REQUIRE(bars->size() == 1);

    const auto& b = bars->front();
    CHECK(b.quality == QualityStatus::stale);
    // The price is kept: it is the last known one and an as-of join needs it.
    // What changes is that nothing downstream can mistake it for a trade.
    CHECK(b.open.value() == 1.16113);
    CHECK(b.volume.value() == 0.0);
}

TEST_CASE("a single-tick minute stays a real observation", "[dukascopy][stale]") {
    // A minute that traded exactly once is also flat, and must NOT be swept up
    // by the rule above. Volume is what separates them, and on the loaded
    // window there were 3,337 such minutes in EURUSD alone.
    std::string raw;
    raw += bar_record(60, 116113, 116113, 116113, 116113, 1.5f);
    auto bars = DukascopyProvider::decode_minute_bars(raw, kDay, 1e-5, PriceBasis::bid);
    REQUIRE(bars.has_value());
    CHECK(bars->front().quality == QualityStatus::valid);
}

TEST_CASE("a minute that moved is valid whatever its volume field says",
          "[dukascopy][stale]") {
    // Guards the rule from the other side: only a FLAT bar can be a carry
    // forward. Verified against the whole loaded window -- of 2.9 million bars
    // there was not one zero-volume bar whose price had moved.
    std::string raw;
    raw += bar_record(120, 116100, 116150, 116090, 116160, 0.0f);
    auto bars = DukascopyProvider::decode_minute_bars(raw, kDay, 1e-5, PriceBasis::bid);
    REQUIRE(bars.has_value());
    CHECK(bars->front().quality == QualityStatus::valid);
}

TEST_CASE("ticks aggregate into minute bars on the requested side", "[dukascopy][ticks]") {
    // The daily candle file does not exist until the day is over, so the
    // current day is built from hourly tick files instead. Without this the
    // service would sit a day behind on every intraday series while the source
    // already had the data.
    std::vector<DukascopyProvider::Tick> ticks;
    auto t = [](int64_t ms, double bid, double ask, double bv, double av) {
        DukascopyProvider::Tick x;
        x.event_time_ms = ms; x.bid = bid; x.ask = ask;
        x.bid_volume = bv; x.ask_volume = av;
        return x;
    };
    // Three ticks in one minute, one in the next.
    ticks.push_back(t(kHour + 1000,  1.1000, 1.1002, 1.0, 2.0));
    ticks.push_back(t(kHour + 20000, 1.1010, 1.1012, 1.5, 2.5));
    ticks.push_back(t(kHour + 50000, 1.0990, 1.0992, 0.5, 1.0));
    ticks.push_back(t(kHour + 70000, 1.1005, 1.1007, 3.0, 4.0));

    const auto bid = DukascopyProvider::bars_from_ticks(ticks, PriceBasis::bid);
    REQUIRE(bid.size() == 2);
    CHECK(bid[0].event_time_ms == kHour);
    CHECK(bid[0].open.value() == 1.1000);
    CHECK(bid[0].high.value() == 1.1010);
    CHECK(bid[0].low.value() == 1.0990);
    CHECK(bid[0].close.value() == 1.0990);
    CHECK(bid[0].volume.value() == 3.0);
    CHECK(bid[0].quality == QualityStatus::valid);
    CHECK(bid[1].event_time_ms == kHour + 60'000);
    CHECK(bid[1].close.value() == 1.1005);

    // The ask side is a different series built from the same ticks.
    const auto ask = DukascopyProvider::bars_from_ticks(ticks, PriceBasis::ask);
    REQUIRE(ask.size() == 2);
    CHECK(ask[0].open.value() == 1.1002);
    CHECK(ask[0].volume.value() == 5.5);
    CHECK(ask[0].close.value() > bid[0].close.value());
}

TEST_CASE("no ticks means no invented bars", "[dukascopy][ticks]") {
    CHECK(DukascopyProvider::bars_from_ticks({}, PriceBasis::bid).empty());
}
