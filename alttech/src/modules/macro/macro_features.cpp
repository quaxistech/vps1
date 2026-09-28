#include "macro_features.hpp"

#include <openssl/sha.h>

#include <iomanip>
#include <sstream>

namespace alttech::modules::macro {

const std::vector<std::string>& macro_feature_names() {
    // clang-format off
    static const std::vector<std::string> names = {
        // --- China: the offshore market, the policy fix, and the gap ---------
        //
        // USDCNH is what the offshore market will actually deal at. The PBOC
        // central parity is where policy says the onshore rate should open. The
        // Fed's H.10 is a third observation of the onshore rate. On any given
        // day all three differ, and the differences are the point.
        "cnh_ret_5m",                 // offshore return, 5 minutes
        "cnh_ret_1h",
        "cnh_ret_1d",
        "cnh_realized_vol_1h",        // stddev of 1m returns over the hour
        "cnh_realized_vol_1d",
        "cnh_spread_bps",             // offshore bid/ask, in basis points
        // The offshore market against the policy setting, in basis points. A
        // positive number means the market is weaker than the fix. It is a
        // measurement, not a verdict.
        "cnh_vs_fixing_bps",
        "cnh_vs_fixing_chg_1d",
        "cnh_vs_fixing_z_20d",        // z-score of the gap over 20 observations
        // The offshore market against the Fed's onshore reference.
        "cnh_vs_h10_bps",
        // The two official observations against each other: policy versus
        // reported. They are different series and this is how different.
        "fixing_vs_h10_bps",
        "fixing_chg_1d",              // daily move of the central parity itself
        "fixing_age_hours",           // how old the newest usable fix is

        // --- FX and metals, intraday where the source is intraday -----------
        "eurusd_ret_1h",
        "eurusd_ret_1d",
        "eurusd_realized_vol_1h",
        "usdjpy_ret_1h",
        "usdjpy_ret_1d",
        "usdjpy_realized_vol_1h",
        "xauusd_ret_1h",
        "xauusd_ret_1d",
        "xauusd_realized_vol_1h",
        "xauusd_realized_vol_1d",

        // --- FX available only daily. No intraday return is fabricated. -----
        "gbpusd_ret_1d",
        "audusd_ret_1d",
        "usdkrw_ret_1d",
        "usdkrw_age_hours",           // daily-only source: staleness is a fact

        // --- Equity and volatility, daily -----------------------------------
        "vix_level",
        "vix_chg_1d",
        "vix_z_20d",
        "spx_ret_1d",
        "ndx_ret_1d",
        "equity_age_hours",

        // --- Rates, daily ----------------------------------------------------
        "us2y_level",
        "us10y_level",
        "us10y_chg_1d",
        "us10y_real_level",
        // Published differences rather than left to be re-derived downstream:
        // both are standard constructions with one correct definition, and
        // computing them here keeps that definition in one place.
        "curve_2s10s",                // 10Y minus 2Y, percentage points
        "breakeven_10y",              // 10Y nominal minus 10Y real
        "rates_age_hours",
    };
    // clang-format on
    return names;
}

size_t macro_feature_count() { return macro_feature_names().size(); }

std::string macro_schema_fingerprint() {
    std::string joined;
    for (const auto& n : macro_feature_names()) {
        joined += n;
        joined.push_back('\n');
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(joined.data()), joined.size(), digest);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char c : digest) out << std::setw(2) << static_cast<int>(c);
    return out.str();
}

} // namespace alttech::modules::macro
