#include "reference_engine.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace alttech::venuelite {
namespace {

// Bars of history needed before the longest indicator (EMA 200) is converged.
constexpr std::size_t kWarmupBars = 200;
// Swing window for the per-venue zone, in 1m bars.
//
// Twenty minutes is a micro-extreme, not a level a 15m decision with a 48h
// horizon can lean on. Three hours is the shortest span over which a high or low
// still means something to an intraday trade. It is a CAP rather than a
// requirement, and the minimum is far lower, because the loaded window is
// `venue_window_len` (220 by default): a hard 240-bar threshold would silently
// stop producing zones altogether on any deployment configured below it.
constexpr std::size_t kSwingWindowMax = 180;
constexpr std::size_t kSwingWindowMin = 30;
// A rejection of at least half the window's average true range is what counts
// as the level being defended; the same fraction the structure module uses.
constexpr double kReactionAtrFraction = 0.5;
constexpr std::size_t kBollingerPeriod = 20;
constexpr int kWilderPeriod = 14;

// Regime boundaries, in units that are comparable across instruments: the
// volatility axis is an empirical percentile in [0,1], the trend axis is the
// EMA spread measured in ATR. A spread of one ATR is the conventional reading
// of "the trend has moved price by a full unit of its own noise".
constexpr double kExpansionPercentile = 0.90;
constexpr double kCompressionPercentile = 0.10;
constexpr double kTrendAtrUnits = 1.0;
constexpr double kBreakoutTrendAtrUnits = 1.5;
constexpr double kLowLiquidityQuality = 0.25;

double ema(const std::vector<double>& v, std::size_t period) {
    if (v.size() < period || period == 0) return std::nan("");
    // Seeded with the SMA of the first `period` samples, as in the standard
    // definition; the recursion then runs over the remaining samples.
    const double k = 2.0 / (static_cast<double>(period) + 1.0);
    double e = std::accumulate(v.begin(), v.begin() + static_cast<long>(period), 0.0) /
               static_cast<double>(period);
    for (std::size_t i = period; i < v.size(); ++i) e = v[i] * k + e * (1.0 - k);
    return e;
}

// Wilder's RSI: the average gain and loss are smoothed with Wilder's
// exponential recursion (alpha = 1/period), seeded with the simple average of
// the first `period` changes. This is TA-Lib's RSI, not the simple-average
// variant.
double wilder_rsi(const std::vector<double>& close, int period) {
    const auto n = static_cast<int>(close.size());
    if (period <= 0 || n < period + 1) return std::nan("");
    double gain = 0.0;
    double loss = 0.0;
    for (int i = 1; i <= period; ++i) {
        const double d = close[static_cast<std::size_t>(i)] - close[static_cast<std::size_t>(i - 1)];
        if (d >= 0.0) gain += d; else loss -= d;
    }
    double avg_gain = gain / period;
    double avg_loss = loss / period;
    const double alpha = 1.0 / period;
    for (int i = period + 1; i < n; ++i) {
        const double d = close[static_cast<std::size_t>(i)] - close[static_cast<std::size_t>(i - 1)];
        const double up = d > 0.0 ? d : 0.0;
        const double down = d < 0.0 ? -d : 0.0;
        avg_gain += alpha * (up - avg_gain);
        avg_loss += alpha * (down - avg_loss);
    }
    if (avg_loss == 0.0) return avg_gain == 0.0 ? 50.0 : 100.0;
    return 100.0 - 100.0 / (1.0 + avg_gain / avg_loss);
}

// Wilder's ATR: the true range smoothed with the same recursion, seeded with
// the simple average of the first `period` true ranges.
double wilder_atr(const std::vector<LiteCandle>& c, int period) {
    const auto n = static_cast<int>(c.size());
    if (period <= 0 || n < period + 1) return std::nan("");
    const auto true_range = [&](std::size_t i) {
        return std::max({c[i].high - c[i].low, std::abs(c[i].high - c[i - 1].close),
                         std::abs(c[i].low - c[i - 1].close)});
    };
    double acc = 0.0;
    for (int i = 1; i <= period; ++i) acc += true_range(static_cast<std::size_t>(i));
    double atr = acc / period;
    const double alpha = 1.0 / period;
    for (int i = period + 1; i < n; ++i) {
        atr += alpha * (true_range(static_cast<std::size_t>(i)) - atr);
    }
    return atr;
}

// Empirical rank of `x` within `sample`, in [0,1]. Distribution-relative, so it
// carries the same meaning for any instrument and any price level.
double percentile_rank(double x, const std::vector<double>& sample) {
    if (sample.empty()) return 0.5;
    std::size_t below = 0;
    for (const double v : sample) {
        if (v <= x) ++below;
    }
    return static_cast<double>(below) / static_cast<double>(sample.size());
}

// Rolling Bollinger bandwidth over the whole window, used as the sample the
// current bandwidth is ranked against.
std::vector<double> bollinger_bandwidth_series(const std::vector<double>& closes) {
    std::vector<double> out;
    if (closes.size() < kBollingerPeriod) return out;
    out.reserve(closes.size() - kBollingerPeriod + 1);
    for (std::size_t end = kBollingerPeriod; end <= closes.size(); ++end) {
        double mean = 0.0;
        for (std::size_t i = end - kBollingerPeriod; i < end; ++i) mean += closes[i];
        mean /= static_cast<double>(kBollingerPeriod);
        if (mean == 0.0) continue;
        double acc = 0.0;
        for (std::size_t i = end - kBollingerPeriod; i < end; ++i) {
            const double d = closes[i] - mean;
            acc += d * d;
        }
        const double sd = std::sqrt(acc / static_cast<double>(kBollingerPeriod));
        out.push_back(4.0 * sd / mean); // (upper - lower) / middle at 2 sigma
    }
    return out;
}

void set_bit(std::vector<uint8_t>& bm, std::size_t i) {
    bm[i / 8] |= static_cast<uint8_t>(1U << (i % 8));
}

} // namespace

std::optional<SwingZone> detect_swing_zone(const std::vector<LiteCandle>& window) {
    if (window.size() < kSwingWindowMin) return std::nullopt;
    const std::size_t span = std::min(window.size(), kSwingWindowMax);
    const std::size_t from = window.size() - span;
    double hi = window[from].high;
    double lo = window[from].low;
    double tr_sum = 0.0;
    for (std::size_t i = from; i < window.size(); ++i) {
        hi = std::max(hi, window[i].high);
        lo = std::min(lo, window[i].low);
        if (i > 0) {
            tr_sum += std::max({window[i].high - window[i].low,
                                std::abs(window[i].high - window[i - 1].close),
                                std::abs(window[i].low - window[i - 1].close)});
        }
    }
    // Band half-width is the average true range of the same window, with a
    // 1 bp floor so a perfectly flat market still yields a non-degenerate band.
    const double band = std::max(tr_sum / static_cast<double>(span), hi * 1e-4);
    const double close = window.back().close;

    SwingZone z;
    z.resistance = (hi - close) <= (close - lo);
    z.center = z.resistance ? hi : lo;
    z.lower = z.center - band;
    z.upper = z.center + band;
    // Evidence that this band is a LEVEL and not just a price. A touch is price
    // entering the band; a reaction is price entering and then being pushed back
    // out by a meaningful amount. Touches alone say nothing — price touches
    // every band it passes through — so the reaction count is what separates a
    // level from a line on a chart, and it is what the consensus gates on.
    //
    // Both are derived from the window on every evaluation. They must never be
    // accumulated across cycles: a counter bumped once per sweep measures the
    // sweep's own cadence, and after four days of that a band price had simply
    // been sitting inside showed 3244 "touches" and zero reactions.
    const double reaction_margin = kReactionAtrFraction * (tr_sum / static_cast<double>(span));
    std::size_t touches = 0;
    std::size_t reactions = 0;
    for (std::size_t i = from; i < window.size(); ++i) {
        if (window[i].low > z.upper || window[i].high < z.lower) continue;
        ++touches;
        // Rejection is measured against the band centre, on the side the level
        // is supposed to defend.
        const double rejection = z.resistance ? z.center - window[i].close
                                              : window[i].close - z.center;
        if (rejection > reaction_margin) ++reactions;
    }
    z.touch_count = static_cast<int>(touches);
    z.reaction_count = static_cast<int>(reactions);
    z.strength = std::clamp(static_cast<double>(touches) / static_cast<double>(span), 0.0, 1.0);
    return z;
}

std::string_view regime_label(VenueRegime r) {
    switch (r) {
        case VenueRegime::trend_up: return "TREND_UP";
        case VenueRegime::trend_down: return "TREND_DOWN";
        case VenueRegime::range: return "RANGE";
        case VenueRegime::breakout: return "BREAKOUT";
        case VenueRegime::volatility_expansion: return "VOLATILITY_EXPANSION";
        case VenueRegime::volatility_compression: return "VOLATILITY_COMPRESSION";
        case VenueRegime::chaotic: return "CHAOTIC";
        case VenueRegime::low_liquidity: return "LOW_LIQUIDITY";
        case VenueRegime::undefined: return "UNDEFINED";
    }
    return "UNDEFINED";
}

// Shared by compute_reference() and compute_bbo_only(), so the primary venue
// and the reference venues derive these from one implementation.
template <typename SetFn>
void fill_bbo_features(const LiteContext& ctx, const SetFn& set) {
    // --- BBO dynamics --------------------------------------------------------
    //
    // Every one of these is left UNSET when the underlying reading is absent.
    // A spread of zero is a statement about the market, not a stand-in for
    // "the venue published no quote this minute".
    //
    // Units: spreads are quoted in basis points of the mid, so they are
    // comparable across instruments whose prices differ by orders of magnitude.
    // The mid used as the denominator is the minute's own (min+max)/2 of the
    // best bid/ask, not the candle close, so the ratio is internally
    // consistent even when the last trade printed away from the quote.
    if (ctx.bbo_spread_min && ctx.bbo_spread_max && ctx.bbo_bid_first && ctx.bbo_ask_first &&
        ctx.bbo_bid_last && ctx.bbo_ask_last) {
        const double mid_first = (*ctx.bbo_bid_first + *ctx.bbo_ask_first) / 2.0;
        const double mid_last = (*ctx.bbo_bid_last + *ctx.bbo_ask_last) / 2.0;
        const double mid = (mid_first + mid_last) / 2.0;
        if (mid > 0.0) {
            const double min_bps = *ctx.bbo_spread_min / mid * 10'000.0;
            const double max_bps = *ctx.bbo_spread_max / mid * 10'000.0;
            // A venue that reports max < min for a minute is inconsistent; the
            // block is skipped rather than published with a negative range.
            if (max_bps >= min_bps && min_bps >= 0.0) {
                const double range_bps = max_bps - min_bps;
                const double mean_bps = (min_bps + max_bps) / 2.0;
                    set(RF_BBO_SPREAD_MIN_BPS, min_bps);
                    set(RF_BBO_SPREAD_MAX_BPS, max_bps);
                    set(RF_BBO_SPREAD_RANGE_BPS, range_bps);
                    set(RF_BBO_SPREAD_MEAN_BPS, mean_bps);
                // How steady the quote was: 1 when the spread never moved, 0
                // when it swung by its own mean or more. Bounded so the value
                // is directly comparable across instruments and venues.
                if (mean_bps > 0.0) {
                    const double stability = 1.0 - range_bps / mean_bps;
                        set(RF_BBO_STABILITY, std::clamp(stability, 0.0, 1.0));
                }
            }
        }
        if (mid_first > 0.0) set(RF_BBO_MID_DRIFT, mid_last / mid_first - 1.0);
    }
    // Quote activity stands on its own: a minute may carry samples without a
    // usable spread pair.
    if (ctx.bbo_sample_count && *ctx.bbo_sample_count >= 0.0) {
        set(RF_BBO_SAMPLE_RATE, *ctx.bbo_sample_count);
    }

}

// Order-book and flow quantities the TA-Lib engine cannot see, shared by the
// reference product and the primary venue's market-only product.
template <typename SetFn>
void fill_market_features(const LiteContext& ctx, double price, const SetFn& set) {
    if (ctx.spread) set(RF_SPREAD, *ctx.spread);
    if (ctx.relative_spread) set(RF_RELATIVE_SPREAD, *ctx.relative_spread);
    if (ctx.book_imbalance) set(RF_BOOK_IMBALANCE, *ctx.book_imbalance);
    // Depth in USDT notional. The reference price is the book's own mid where
    // the book reported one, falling back to the bar close: a depth figure
    // must be denominated in the same market the levels were quoted in.
    if (ctx.bid_depth5 && ctx.ask_depth5) {
        const double ref = (ctx.mid_price && *ctx.mid_price > 0.0) ? *ctx.mid_price : price;
        if (ref > 0.0) set(RF_DEPTH_USDT, (*ctx.bid_depth5 + *ctx.ask_depth5) * ref);
    }
    // --- intra-minute flow and liquidity -------------------------------------
    if (ctx.flow_peak_trades_1s) set(RF_FLOW_PEAK_TRADES_1S, *ctx.flow_peak_trades_1s);
    if (ctx.flow_peak_volume_1s) set(RF_FLOW_PEAK_VOLUME_1S, *ctx.flow_peak_volume_1s);
    // Burst ratio separates a minute whose volume arrived in one second from a
    // minute with the same total spread evenly across it. 1.0 means perfectly
    // even; higher means concentrated.
    if (ctx.flow_peak_volume_1s && ctx.flow_mean_volume_1s && *ctx.flow_mean_volume_1s > 0.0) {
        set(RF_FLOW_BURST_RATIO, *ctx.flow_peak_volume_1s / *ctx.flow_mean_volume_1s);
    }
    if (ctx.flow_active_seconds) {
        set(RF_FLOW_ACTIVE_SECONDS, std::clamp(*ctx.flow_active_seconds, 0.0, 1.0));
    }
    if (ctx.flow_signed_stddev) set(RF_FLOW_SIGNED_DISPERSION, *ctx.flow_signed_stddev);
    if (ctx.book_depth_min_1s) set(RF_BOOK_DEPTH_MIN, *ctx.book_depth_min_1s);
    // How far liquidity withdrew inside the minute: 0 when depth never dipped,
    // approaching 1 when the book briefly emptied. A book that is merely thin
    // all minute scores 0 -- this measures the WITHDRAWAL, not the level.
    if (ctx.book_depth_min_1s && ctx.book_depth_mean_1s && *ctx.book_depth_mean_1s > 0.0) {
        set(RF_BOOK_DEPTH_DROP,
            std::clamp(1.0 - *ctx.book_depth_min_1s / *ctx.book_depth_mean_1s, 0.0, 1.0));
    }

    // --- derivatives ---------------------------------------------------------
    //
    // These come from market_derivatives_context_1m, which the TA-Lib engine
    // cannot see: it works from candles. Without them here the execution venue
    // -- whose product is market-only -- had no mark/index basis, no funding
    // and no open-interest change at all, while the three information venues
    // did.
    if (ctx.mark_price && price > 0.0) set(RF_MARK_DEVIATION, *ctx.mark_price / price - 1.0);
    if (ctx.index_price && price > 0.0) set(RF_INDEX_DEVIATION, *ctx.index_price / price - 1.0);
    // Funding is quoted per settlement interval and the interval differs per
    // venue, so it is normalised to 8 hours to be comparable.
    if (ctx.funding_rate && ctx.funding_interval_hours && *ctx.funding_interval_hours > 0.0) {
        set(RF_FUNDING_8H, *ctx.funding_rate / *ctx.funding_interval_hours * 8.0);
    }
    if (ctx.oi_change_ratio) set(RF_OI_CHANGE, *ctx.oi_change_ratio);
}


RefResult compute_bbo_only(const LiteContext& ctx) {
    RefResult r;
    r.values.assign(RF_FEATURE_COUNT, 0.0F);
    r.validity.assign((RF_FEATURE_COUNT + 7) / 8, 0);
    const auto set = [&](std::size_t idx, double value) {
        if (!std::isfinite(value)) return;
        r.values[idx] = static_cast<float>(value);
        r.validity[idx / 8] |= static_cast<uint8_t>(1U << (idx % 8));
    };
    fill_bbo_features(ctx, set);
    // No candle is loaded for this product, so the book's own mid is the only
    // reference price available -- which is the correct one for depth anyway.
    fill_market_features(ctx, ctx.mid_price.value_or(0.0), set);
    // Data quality still reports itself: a consumer must be able to tell "no
    // quote this minute" from "this vector carries no BBO block by design".
    set(RF_DATA_QUALITY, ctx.data_quality);
    // Never WARMING: this product needs no candle history at all.
    r.warming = false;
    return r;
}

RefResult compute_reference(const std::vector<LiteCandle>& window, const LiteContext& ctx) {
    RefResult r;
    r.values.assign(RF_FEATURE_COUNT, 0.0F);
    r.validity.assign((RF_FEATURE_COUNT + 7) / 8, 0);
    if (window.empty()) {
        r.warming = true;
        return r;
    }
    const auto set = [&](std::size_t idx, double v) {
        if (!std::isfinite(v)) return;
        r.values[idx] = static_cast<float>(v);
        set_bit(r.validity, idx);
    };

    std::vector<double> closes;
    closes.reserve(window.size());
    for (const auto& c : window) closes.push_back(c.close);
    const double close = closes.back();

    if (closes.size() >= 2 && closes[closes.size() - 2] > 0.0 && close > 0.0) {
        const double prev = closes[closes.size() - 2];
        set(RF_RETURN, close / prev - 1.0);
        set(RF_LOG_RETURN, std::log(close / prev));
    }

    std::vector<double> logret;
    logret.reserve(closes.size());
    for (std::size_t i = 1; i < closes.size(); ++i) {
        if (closes[i - 1] > 0.0 && closes[i] > 0.0) {
            logret.push_back(std::log(closes[i] / closes[i - 1]));
        }
    }

    const double atr14 = wilder_atr(window, kWilderPeriod);
    set(RF_ATR_14, atr14);
    set(RF_RSI_14, wilder_rsi(closes, kWilderPeriod));

    const double e20 = ema(closes, 20);
    const double e50 = ema(closes, 50);
    const double e200 = ema(closes, 200);
    set(RF_EMA_20, e20);
    set(RF_EMA_50, e50);
    set(RF_EMA_200, e200);
    if (std::isfinite(e20) && std::isfinite(e50) && std::isfinite(e200)) {
        double alignment = 0.0;
        if (e20 > e50 && e50 > e200) alignment = 1.0;
        else if (e20 < e50 && e50 < e200) alignment = -1.0;
        set(RF_EMA_ALIGNMENT, alignment);
    }
    if (closes.size() >= 21 && std::isfinite(atr14) && atr14 > 0.0) {
        const std::vector<double> previous(closes.begin(), closes.end() - 1);
        const double e20_prev = ema(previous, 20);
        if (std::isfinite(e20) && std::isfinite(e20_prev)) {
            // Slope in ATR units: comparable across instruments and regimes.
            set(RF_EMA_SLOPE, (e20 - e20_prev) / atr14);
        }
    }

    if (closes.size() >= 11) {
        const double past = closes[closes.size() - 11];
        set(RF_MOMENTUM_10, close - past);
        if (past > 0.0) set(RF_ROC_10, close / past - 1.0);
    }

    if (logret.size() >= 20) {
        const std::vector<double> tail(logret.end() - 20, logret.end());
        const double mean = std::accumulate(tail.begin(), tail.end(), 0.0) / 20.0;
        double acc = 0.0;
        for (const double x : tail) acc += (x - mean) * (x - mean);
        set(RF_REALIZED_VOL_20, std::sqrt(acc / 20.0));
    }

    const auto bandwidths = bollinger_bandwidth_series(closes);
    double bb_width = std::nan("");
    if (!bandwidths.empty()) {
        bb_width = bandwidths.back();
        set(RF_BB_WIDTH_20, bb_width);
        set(RF_BB_WIDTH_PERCENTILE, percentile_rank(bb_width, bandwidths));
    }

    if (window.size() >= 20) {
        std::vector<double> volume;
        std::vector<double> turnover;
        for (std::size_t i = window.size() - 20; i < window.size(); ++i) {
            volume.push_back(window[i].base_volume);
            turnover.push_back(window[i].quote_volume);
        }
        const auto zscore = [](const std::vector<double>& s, double x) -> double {
            const double mean = std::accumulate(s.begin(), s.end(), 0.0) /
                                static_cast<double>(s.size());
            double acc = 0.0;
            for (const double v : s) acc += (v - mean) * (v - mean);
            const double sd = std::sqrt(acc / static_cast<double>(s.size()));
            return sd > 0.0 ? (x - mean) / sd : std::nan("");
        };
        set(RF_VOLUME_ZSCORE_20, zscore(volume, window.back().base_volume));
        set(RF_TURNOVER_ZSCORE_20, zscore(turnover, window.back().quote_volume));
    }

    // Cumulative volume delta over the whole window, from the candles' own
    // taker split. USDT notional, so it is comparable across instruments.
    {
        double cvd = 0.0;
        bool any = false;
        for (const auto& c : window) {
            if (c.taker_buy_base_volume == 0.0 && c.taker_sell_base_volume == 0.0) continue;
            cvd += (c.taker_buy_base_volume - c.taker_sell_base_volume) * c.close;
            any = true;
        }
        if (any) set(RF_CVD, cvd);
        const auto& last = window.back();
        if (last.taker_buy_base_volume != 0.0 || last.taker_sell_base_volume != 0.0) {
            set(RF_SIGNED_VOLUME,
                (last.taker_buy_base_volume - last.taker_sell_base_volume) * last.close);
        }
    }

    // --- context measured directly by AltSpotter's 1m aggregates -------------
    if (ctx.vwap && *ctx.vwap > 0.0) set(RF_VWAP_RELATION, close / *ctx.vwap - 1.0);
    if (ctx.buy_volume && ctx.sell_volume) {
        const double b = *ctx.buy_volume;
        const double s = *ctx.sell_volume;
        if (b + s > 0.0) set(RF_TRADE_IMBALANCE, (b - s) / (b + s));
    }
    if (ctx.signed_volume_base) {
        set(RF_SIGNED_VOLUME, *ctx.signed_volume_base * close);
    }
    if (ctx.trade_count) set(RF_TRADE_INTENSITY, *ctx.trade_count);
    // Spread, book imbalance and the intra-minute flow/liquidity block are
    // shared with the primary venue's product -- one implementation, so the
    // same quantity is never derived twice in two places.
    fill_market_features(ctx, close, set);
    if (ctx.microprice && ctx.mid_price && *ctx.mid_price > 0.0) {
        set(RF_MICROPRICE_REL, *ctx.microprice / *ctx.mid_price - 1.0);
    }
    set(RF_DATA_QUALITY, ctx.data_quality);

    fill_bbo_features(ctx, set);


    r.warming = closes.size() < kWarmupBars;

    // --- deterministic venue regime -----------------------------------------
    r.liquidity_quality = ctx.data_quality;
    const double volatility_percentile =
        bandwidths.empty() ? std::nan("") : percentile_rank(bb_width, bandwidths);
    r.volatility_regime = std::isfinite(volatility_percentile) ? volatility_percentile : 0.0;
    // Trend measured as the EMA(20)-EMA(200) spread in ATR units: how far the
    // trend has carried price relative to the market's own noise.
    double trend = 0.0;
    if (std::isfinite(e20) && std::isfinite(e200) && std::isfinite(atr14) && atr14 > 0.0) {
        trend = (e20 - e200) / atr14;
    }
    r.trend_strength = trend;

    // Order matters. Volatility expansion is checked first because it is the
    // most actionable state and it subsumes direction (a breakout is an
    // expansion that also has direction). A clear trend is checked next, ahead
    // of compression: a market that has moved by more than an ATR of its own
    // noise is trending, even if its *relative* bandwidth happens to be low.
    // Compression only describes a market that is going nowhere quietly.
    if (ctx.data_quality < kLowLiquidityQuality) {
        r.regime = VenueRegime::low_liquidity;
        r.regime_confidence = 0.5;
    } else if (!std::isfinite(volatility_percentile) || r.warming) {
        r.regime = VenueRegime::undefined;
        r.regime_confidence = 0.0;
    } else if (volatility_percentile > kExpansionPercentile) {
        r.regime = std::abs(trend) > kBreakoutTrendAtrUnits ? VenueRegime::breakout
                                                            : VenueRegime::volatility_expansion;
        r.regime_confidence = std::min(1.0, volatility_percentile);
    } else if (std::abs(trend) > kTrendAtrUnits) {
        r.regime = trend > 0.0 ? VenueRegime::trend_up : VenueRegime::trend_down;
        r.regime_confidence = std::min(1.0, std::abs(trend) / (2.0 * kTrendAtrUnits));
    } else if (volatility_percentile < kCompressionPercentile) {
        r.regime = VenueRegime::volatility_compression;
        r.regime_confidence = std::min(1.0, 1.0 - volatility_percentile);
    } else {
        r.regime = VenueRegime::range;
        r.regime_confidence = 0.5;
    }
    return r;
}

} // namespace alttech::venuelite
