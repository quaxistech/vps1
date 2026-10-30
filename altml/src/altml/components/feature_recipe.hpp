#pragma once

// The model's feature space: which columns of each AltTech set a model reads,
// and the few quantities derived from them. Pure; shared by the trainer and
// inference, so the served vector is the trained vector by construction.
//
// WHY A PROJECTION AND NOT ALL 995 COLUMNS
//
// talib-core publishes every TA-Lib function over seven periods. Roughly half
// of those columns are PRICE LEVELS -- SMA/EMA/KAMA/T3/BBANDS/MIDPOINT/MAX/MIN,
// linear-regression intercepts, TYPPRICE -- in the instrument's own units. In a
// model pooled across 27 instruments a level identifies the instrument (BTC's
// EMA is 60,000; DOGE's is 0.1) and, within one instrument, the calendar
// period (the price was 64k in this fold and 58k in that one). A tree that
// splits on a level is learning which month it is. The same holds for
// cumulative or volume-scaled columns (OBV, AD, CVD, depth in base units).
//
// So a talib-core source contributes the scale-free readings -- oscillators,
// returns, normalised volatility, trend strength, cycle phase -- plus a small
// set of DERIVED relations that turn the useful levels into geometry: distance
// to the moving averages and position in the Donchian channel, both in the
// instrument's own volatility units. The strategies test exactly these
// relations by hand ("pullback of 0.8-3 ATR below EMA20", "break of the prior
// 20-bar high"); here they are inputs, and the model decides what matters.
//
// Every derived value is NaN when any input is missing or a denominator is not
// positive. Missing stays missing -- never 0, never "neutral".

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace altml::recipe {

inline constexpr const char* kRecipeVersion = "recipe-1";

// Scale-free talib-core columns: bounded oscillators, fractional returns,
// normalised ATR, directional-movement strength, cycle readings.
inline const std::vector<std::string>& talib_projection() {
    static const std::vector<std::string> names = {
        "NATR_14_outReal", "NATR_50_outReal", "NATR_200_outReal",
        "ROCP_5_outReal", "ROCP_14_outReal", "ROCP_50_outReal", "ROCP_200_outReal",
        "RSI_14_outReal", "RSI_50_outReal", "CCI_20_outReal", "CMO_14_outReal",
        "WILLR_14_outReal", "MFI_14_outReal", "ULTOSC_outReal", "STOCH_SMA_outSlowK",
        "BOP_outReal", "AROONOSC_14_outReal", "AROONOSC_50_outReal", "IMI_14_outReal",
        "ADX_14_outReal", "ADX_50_outReal", "PLUS_DI_14_outReal", "MINUS_DI_14_outReal",
        "PPO_EMA_outReal", "TRIX_14_outReal", "TRIX_50_outReal",
        "HT_TRENDMODE_outInteger", "HT_DCPERIOD_outReal", "HT_DCPHASE_outReal",
    };
    return names;
}

// recipe-2 (2026-10-30, DT-02/DT-04): recipe-1 plus the families talib-core
// already computes and recipe-1 left out, each scale-free by construction:
//   * candlestick integer strengths (normally -100/0/+100; confirmed Hikkake
//     can also be -200/+200), causal on the closed bar they describe;
//   * further bounded oscillators and fractional/normalised readings.
// A model records the recipe it was trained on; inference serves each model
// the projection of ITS recipe, so a recipe-1 model never receives recipe-2
// columns. Which columns a recipe-2 model actually uses is the trainer's
// selection on its own folds -- the recipe only makes them available.
inline constexpr const char* kRecipeVersion2 = "recipe-2";

inline const std::vector<std::string>& candlestick_projection() {
    static const std::vector<std::string> names = {
        "CDL2CROWS_outInteger", "CDL3BLACKCROWS_outInteger", "CDL3INSIDE_outInteger",
        "CDL3LINESTRIKE_outInteger", "CDL3OUTSIDE_outInteger", "CDL3STARSINSOUTH_outInteger",
        "CDL3WHITESOLDIERS_outInteger", "CDLABANDONEDBABY_outInteger", "CDLADVANCEBLOCK_outInteger",
        "CDLBELTHOLD_outInteger", "CDLBREAKAWAY_outInteger", "CDLCLOSINGMARUBOZU_outInteger",
        "CDLCONCEALBABYSWALL_outInteger", "CDLCOUNTERATTACK_outInteger", "CDLDARKCLOUDCOVER_outInteger",
        "CDLDOJI_outInteger", "CDLDOJISTAR_outInteger", "CDLDRAGONFLYDOJI_outInteger",
        "CDLENGULFING_outInteger", "CDLEVENINGDOJISTAR_outInteger", "CDLEVENINGSTAR_outInteger",
        "CDLGAPSIDESIDEWHITE_outInteger", "CDLGRAVESTONEDOJI_outInteger", "CDLHAMMER_outInteger",
        "CDLHANGINGMAN_outInteger", "CDLHARAMI_outInteger", "CDLHARAMICROSS_outInteger",
        "CDLHIGHWAVE_outInteger", "CDLHIKKAKE_outInteger", "CDLHIKKAKEMOD_outInteger",
        "CDLHOMINGPIGEON_outInteger", "CDLIDENTICAL3CROWS_outInteger", "CDLINNECK_outInteger",
        "CDLINVERTEDHAMMER_outInteger", "CDLKICKING_outInteger", "CDLKICKINGBYLENGTH_outInteger",
        "CDLLADDERBOTTOM_outInteger", "CDLLONGLEGGEDDOJI_outInteger", "CDLLONGLINE_outInteger",
        "CDLMARUBOZU_outInteger", "CDLMATCHINGLOW_outInteger", "CDLMATHOLD_outInteger",
        "CDLMORNINGDOJISTAR_outInteger", "CDLMORNINGSTAR_outInteger", "CDLONNECK_outInteger",
        "CDLPIERCING_outInteger", "CDLRICKSHAWMAN_outInteger", "CDLRISEFALL3METHODS_outInteger",
        "CDLSEPARATINGLINES_outInteger", "CDLSHOOTINGSTAR_outInteger", "CDLSHORTLINE_outInteger",
        "CDLSPINNINGTOP_outInteger", "CDLSTALLEDPATTERN_outInteger", "CDLSTICKSANDWICH_outInteger",
        "CDLTAKURI_outInteger", "CDLTASUKIGAP_outInteger", "CDLTHRUSTING_outInteger",
        "CDLTRISTAR_outInteger", "CDLUNIQUE3RIVER_outInteger", "CDLUPSIDEGAP2CROWS_outInteger",
        "CDLXSIDEGAP3METHODS_outInteger",
    };
    return names;
}

// Bounded or fractional talib-core readings recipe-1 did not take (DT-04).
inline const std::vector<std::string>& extra_scale_free_projection() {
    static const std::vector<std::string> names = {
        "RSI_5_outReal", "RSI_200_outReal", "CCI_14_outReal", "CCI_50_outReal",
        "WILLR_50_outReal", "MFI_50_outReal", "CMO_50_outReal", "ROCP_20_outReal",
        "AROON_14_outAroonUp", "AROON_14_outAroonDown", "ADXR_14_outReal",
        "DX_14_outReal", "STOCHF_SMA_outFastK", "STOCHRSI_14_SMA_outFastK", "HT_TRENDMODE_outInteger",
        "HT_SINE_outSine", "HT_SINE_outLeadSine", "CORREL_14_outReal", "BETA_14_outReal",
    };
    return names;
}

// The talib-core projection of a recipe version; empty for one this build
// does not know (the caller refuses it).
[[nodiscard]] inline std::vector<std::string> projection_for(std::string_view recipe_version) {
    if (recipe_version == kRecipeVersion) return talib_projection();
    if (recipe_version != kRecipeVersion2) return {};
    std::vector<std::string> names = talib_projection();
    const std::set<std::string> taken(names.begin(), names.end());
    for (const auto* family : {&candlestick_projection(), &extra_scale_free_projection()}) {
        for (const auto& n : *family) {
            if (!taken.contains(n)) names.push_back(n);
        }
    }
    return names;
}

// Columns of the other sets that are levels, cumulative sums or quantities in
// the instrument's base units -- the same objection as above -- or, for the
// macro block, levels and publication ages that inside a few weeks of history
// only identify the date. Everything else in those sets is taken whole.
inline const std::set<std::string>& excluded_columns() {
    static const std::set<std::string> names = {
        "cross_venue_core:consensus_mid_price", "cross_venue_core:consensus_mark_price",
        "cross_venue_core:consensus_index_price",
        "bitget_micro:aggressive_buy_volume", "bitget_micro:aggressive_sell_volume",
        "bitget_micro:signed_volume", "bitget_micro:delta", "bitget_micro:cvd",
        "bitget_micro:average_trade_size", "bitget_micro:median_trade_size",
        "bitget_micro:max_trade_size", "bitget_micro:buy_trade_count",
        "bitget_micro:sell_trade_count", "bitget_micro:vwap", "bitget_micro:book_bid_depth",
        "bitget_micro:book_ask_depth", "bitget_micro:book_sample_count",
        "reference_lite:ema_20", "reference_lite:ema_50", "reference_lite:ema_200",
        "reference_lite:atr_14", "reference_lite:momentum_10", "reference_lite:signed_volume",
        "reference_lite:cvd", "reference_lite:spread", "reference_lite:depth_usdt",
        "reference_lite_micro:signed_volume", "reference_lite_micro:cvd",
        "reference_lite_micro:spread", "reference_lite_micro:bid_depth5_usdt",
        "reference_lite_micro:ask_depth5_usdt",
        "macro-core:us2y_level", "macro-core:us10y_level", "macro-core:us10y_real_level",
        "macro-core:curve_2s10s", "macro-core:breakeven_10y", "macro-core:vix_level",
        "macro-core:fixing_age_hours", "macro-core:usdkrw_age_hours",
        "macro-core:equity_age_hours", "macro-core:rates_age_hours",
    };
    return names;
}

[[nodiscard]] inline bool is_excluded(std::string_view set, std::string_view column) {
    // Real queue OFI is in base units and raw depth depends on the instrument.
    // Only the producer's dimensionless, continuity-qualified ratio is a model
    // feature; the remaining fields retain observation/quality provenance.
    if (set == "book_ofi_receipt") return column != "ofi_depth_normalized";
    return excluded_columns().contains(std::string(set) + ":" + std::string(column));
}

[[nodiscard]] inline bool source_layout_compatible(std::string_view set, int version,
                                                   const std::vector<std::string>& columns) {
    if (set != "book_ofi_receipt") return !columns.empty();
    return version == 1 && columns == std::vector<std::string>{
        "ofi_base", "ofi_depth_normalized", "ofi_event_count", "ofi_mean_depth_base"};
}

[[nodiscard]] inline bool source_method_compatible(std::string_view recipe_version,
    std::string_view set, std::string_view engine) {
    if (recipe_version != kRecipeVersion2 || set == "talib-core") return true;
    // New family inputs must name the producing method in the persisted
    // recipe, even when its column layout did not change across a repair.
    if (engine.empty()) return false;
    if (set == "macro-core") return engine == "mc-1.4.0";
    if (set == "book_ofi_receipt") return engine == "alttech-ofi-1";
    if (set == "bitget_micro") return engine == "alttech-micro-1";
    if (set == "reference_lite" || set == "reference_lite_micro") return engine == "alttech-reference-1";
    if (set == "cross_venue_core") return engine == "xv-1.0.0";
    return false; // a new typed family needs an explicit adapter/contract
}

// ------------------------------------------------------------------ derived

inline const std::vector<std::string>& derived_names() {
    static const std::vector<std::string> names = {
        "dist_ema20_atr", "dist_ema50_atr", "dist_ema200_atr",
        "don_pos_20", "don_pos_50", "don_pos_200",
        "natr_ratio_14_200", "di_diff_14", "macd_hist_atr",
        "vol_ratio_5_50", "vol_ratio_50_200", "stddev5_atr",
        "slope14_atr", "slope50_atr", "ema_stack",
    };
    return names;
}

// Positions, in one talib-core layout, of every input the derived block reads.
struct DerivedRecipe {
    enum In : size_t {
        kWcl, kMed, kEma20, kEma50, kEma200, kAtr14, kHi20, kLo20, kHi50, kLo50, kHi200, kLo200,
        kNatr14, kNatr200, kPdi14, kMdi14, kMacdHist, kVol5, kVol50, kVol200, kStd5, kSlope14,
        kSlope50, kCount
    };
    std::array<size_t, kCount> pos{};
    bool ok = false;

    static const std::array<const char*, kCount>& input_names() {
        static const std::array<const char*, kCount> n = {
            "WCLPRICE_outReal", "MEDPRICE_outReal", "EMA_20_outReal", "EMA_50_outReal",
            "EMA_200_outReal", "ATR_14_outReal", "MAX_HIGH_20_outReal", "MIN_LOW_20_outReal",
            "MAX_HIGH_50_outReal", "MIN_LOW_50_outReal", "MAX_HIGH_200_outReal",
            "MIN_LOW_200_outReal", "NATR_14_outReal", "NATR_200_outReal", "PLUS_DI_14_outReal",
            "MINUS_DI_14_outReal", "MACD_outMACDHist", "SMA_VOL_5_outReal", "SMA_VOL_50_outReal",
            "SMA_VOL_200_outReal", "STDDEV_5_outReal", "LINEARREG_SLOPE_14_outReal",
            "LINEARREG_SLOPE_50_outReal"};
        return n;
    }

    // Resolve against a layout; unresolved (ok = false) if any input is absent,
    // which the caller treats as a refusal, never as a silently narrower block.
    [[nodiscard]] static DerivedRecipe resolve(const std::vector<std::string>& layout) {
        DerivedRecipe r;
        for (size_t k = 0; k < kCount; ++k) {
            bool found = false;
            for (size_t i = 0; i < layout.size(); ++i) {
                if (layout[i] == input_names()[k]) { r.pos[k] = i; found = true; break; }
            }
            if (!found) return r;
        }
        r.ok = true;
        return r;
    }

    [[nodiscard]] static size_t width() { return derived_names().size(); }

    // `v` is one full vector in the resolved layout, invalid slots already NaN.
    void compute(const float* v, size_t n, float* out) const {
        constexpr float nan = std::numeric_limits<float>::quiet_NaN();
        const auto at = [&](In k) -> double {
            const size_t p = pos[k];
            return p < n ? static_cast<double>(v[p]) : std::nan("");
        };
        const auto ratio = [](double a, double b) -> float {
            if (!std::isfinite(a) || !std::isfinite(b) || !(b > 0)) return nan;
            return static_cast<float>(a / b);
        };
        const double close = 2.0 * at(kWcl) - at(kMed);   // WCL = (H + L + 2C) / 4
        const double atr = at(kAtr14);
        size_t o = 0;
        for (const In e : {kEma20, kEma50, kEma200}) out[o++] = ratio(close - at(e), atr);
        for (const auto& [hi, lo] : {std::pair{kHi20, kLo20}, std::pair{kHi50, kLo50},
                                    std::pair{kHi200, kLo200}}) {
            out[o++] = ratio(close - at(lo), at(hi) - at(lo));
        }
        out[o++] = ratio(at(kNatr14), at(kNatr200));
        {
            const double d = at(kPdi14) - at(kMdi14);
            out[o++] = std::isfinite(d) ? static_cast<float>(d) : nan;
        }
        out[o++] = ratio(at(kMacdHist), atr);
        out[o++] = ratio(at(kVol5), at(kVol50));
        out[o++] = ratio(at(kVol50), at(kVol200));
        out[o++] = ratio(at(kStd5), atr);
        out[o++] = ratio(at(kSlope14), atr);
        out[o++] = ratio(at(kSlope50), atr);
        {
            const double e20 = at(kEma20), e50 = at(kEma50), e200 = at(kEma200);
            if (!std::isfinite(close) || !std::isfinite(e20) || !std::isfinite(e50) ||
                !std::isfinite(e200)) {
                out[o++] = nan;
            } else if (close > e20 && e20 > e50 && e50 > e200) {
                out[o++] = 1.0f;
            } else if (close < e20 && e20 < e50 && e50 < e200) {
                out[o++] = -1.0f;
            } else {
                out[o++] = 0.0f;   // measured: the averages are not stacked
            }
        }
    }
};

// ------------------------------------------------------------------ naming

// Column names carry their provenance -- venue, grid, set -- and are the same
// strings in the trainer and in inference: the model's registry row stores the
// SHA-256 of the joined list, and a served model whose rebuilt list hashes
// differently is refused.
[[nodiscard]] inline std::string source_prefix(int exchange_id, std::string_view timeframe,
                                               std::string_view set) {
    return "v" + std::to_string(exchange_id) + "_" + std::string(timeframe) + "_" +
           std::string(set) + "_";
}

inline void append_base_names(const std::vector<std::string>& projection,
                              std::vector<std::string>& out) {
    for (const auto& n : projection) out.push_back("b_" + n);
    for (const auto& n : derived_names()) out.push_back("b_d_" + n);
}

inline void append_source_names(const std::string& prefix, const std::vector<std::string>& projection,
                                bool derived, std::vector<std::string>& out) {
    for (const auto& n : projection) out.push_back(prefix + n);
    if (derived) {
        for (const auto& n : derived_names()) out.push_back(prefix + "d_" + n);
    }
}

[[nodiscard]] inline std::string joined_names(const std::vector<std::string>& names) {
    std::string joined;
    for (const auto& n : names) { joined += n; joined += '\n'; }
    return joined;
}

} // namespace altml::recipe
