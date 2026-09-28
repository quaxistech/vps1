#include "macro_availability.hpp"

namespace alttech::modules::macro {
namespace {
constexpr int64_t kHour = 3'600'000;
constexpr int64_t kDay = 86'400'000;

// 0 = Sunday ... 6 = Saturday. 1970-01-01 was a Thursday.
int weekday_utc(int64_t ms) noexcept {
    int64_t days = ms / kDay;
    if (ms < 0 && ms % kDay != 0) --days;
    return static_cast<int>(((days % 7) + 7 + 4) % 7);
}

// Start of the observation's own day: callers pass day starts, but a value
// that arrives with an intraday timestamp must not shift the rule.
int64_t day_start(int64_t ms) noexcept {
    int64_t d = ms / kDay;
    if (ms < 0 && ms % kDay != 0) --d;
    return d * kDay;
}

bool is_h10(std::string_view code) noexcept {
    return code == "REF.USDCNY.FED" || code == "REF.USDKRW.FED" ||
           code == "REF.GBPUSD.FED" || code == "REF.AUDUSD.FED" ||
           code == "REF.USDJPY.FED" || code == "REF.EURUSD.FED";
}

bool is_fred_daily(std::string_view code) noexcept {
    return code == "RATE.US.2Y" || code == "RATE.US.10Y" || code == "RATE.US.10Y.REAL" ||
           code == "IDX.VIX" || code == "IDX.SPX" || code == "IDX.NDX";
}
} // namespace

int64_t available_at_ms(std::string_view code, int64_t observation_start_ms) noexcept {
    const int64_t day = day_start(observation_start_ms);
    const int dow = weekday_utc(day);

    // The PBOC parity is set the same morning; the aggregator has it by
    // mid-morning UTC.
    if (code == "REF.USDCNY.PBOC") return day + 8 * kHour;

    // FRED daily series: the day after the next business day.
    if (is_fred_daily(code)) {
        switch (dow) {
            case 5:  return day + 4 * kDay;   // Friday   -> Tuesday
            case 6:  return day + 3 * kDay;   // Saturday -> Tuesday
            default: return day + 2 * kDay;   // Sun-Thu  -> two days on
        }
    }

    // H.10 is weekly: the Tuesday after the Monday release that covers the
    // observation's week (Monday to Friday).
    if (is_h10(code)) {
        switch (dow) {
            case 1:  return day + 8 * kDay;   // Monday
            case 2:  return day + 7 * kDay;
            case 3:  return day + 6 * kDay;
            case 4:  return day + 5 * kDay;
            case 5:  return day + 4 * kDay;   // Friday
            case 6:  return day + 10 * kDay;  // a weekend observation would fall
            default: return day + 9 * kDay;   // into the following week's release
        }
    }

    // An unknown series is treated as the slowest thing in the block rather
    // than the fastest: a source added later must prove it is quick, not be
    // assumed so.
    return day + 8 * kDay;
}

int64_t intraday_cutoff_ms(int64_t t_ms) noexcept {
    constexpr int64_t kPublicationGrace = 30 * 60'000;
    const int64_t shifted = t_ms - kPublicationGrace;
    int64_t h = shifted / kHour;
    if (shifted < 0 && shifted % kHour != 0) --h;
    return h * kHour;
}

} // namespace alttech::modules::macro
