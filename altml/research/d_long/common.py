"""D_long_v1: shared definitions.

A versioned, point-in-time market-state dataset reconstructed from AltSpotter's
raw 1m candles (all four venues) with AltTech's own engine (alttech-dataset),
plus derivatives, book, macro and AltStrategy's real candidates where they
exist. Every sample is (instrument, decision time tau) and holds only what was
known at tau. Outcomes (returns, MFE/MAE, realised costs) are labels and never
enter a feature block.

Nothing here reads a live database: the inputs are the raw snapshot under
/var/lib/altml/datasets/D_long_v1 (checksummed), so the dataset does not depend
on operational retention.
"""

import numpy as np

DATASET_VERSION = "D_long_v1"
CUTOFF_MS = 1_790_294_400_000          # 2026-09-25 00:00:00 UTC, snapshot cutoff
MIN_MS = 60_000
BITGET, BINANCE, OKX, BYBIT = 1, 2, 3, 4
VENUES = (BITGET, BINANCE, OKX, BYBIT)
MONTHS = ("202512", "202602", "202604", "202605", "202606", "202607", "202608", "202609")

# Decision grid: every 5 minutes, each instrument sampled every 15 minutes
# with its own phase, so the 15m/1h staleness of the aligned blocks covers
# every phase production meets (it scores every minute).
GRID_MS = 5 * MIN_MS
PHASES = 3

# Decision time tau = bar close + 5 s: AltML publishes on the Bitget 1m vector
# 2-4 s after the close (measured p50 2 s / p95 4 s) and scores in ~2 s.
DECISION_DELAY_MS = 5_000

# Availability of a closed bar on each grid: close + measured p90 of AltTech's
# first-computation delay (live rows of the AltML archive, 2026-09-15..25).
# p90 rather than p50 so the dataset never sees a bar earlier than production
# usually would; the heavy tail (15m p90 21 min) is itself a finding.
AVAIL_LAG_MS = {"1m": 4_000, "5m": 383_000, "15m": 1_249_000, "30m": 1_478_000,
                "1h": 1_491_000, "4h": 2_583_000, "1d": 3_973_000}
SPAN_MS = {"1m": MIN_MS, "5m": 5 * MIN_MS, "15m": 15 * MIN_MS, "30m": 30 * MIN_MS,
           "1h": 60 * MIN_MS, "4h": 240 * MIN_MS, "1d": 1440 * MIN_MS}
# Same bounds production's aligned sources use where they exist (15m 60, 1h
# 180); the others scale with the span.
FRESH_MS = {"1m": 0, "5m": 20 * MIN_MS, "15m": 60 * MIN_MS, "30m": 120 * MIN_MS,
            "1h": 180 * MIN_MS, "4h": 720 * MIN_MS, "1d": 2880 * MIN_MS}

HORIZONS = {"15m": 15, "1h": 60, "4h": 240}
# Entry latency scenarios, minutes after the decision bar's close: 0 = the
# close itself (ideal), 1 = the ML path (tau + execution), 3 = the candidate
# path as measured (context p50 134 s), 7 = its p95.
LATENCIES = (0, 1, 3, 7)

# Cost model v2 (see cost_model_v2 in build.py): fee from altml.fee_models v1
# (taker 6 bps per side, both legs taker); spread OBSERVED at tau when a book
# reading exists, never a future one; impact an explicit assumption.
FEE_BPS_PER_SIDE = 6.0
IMPACT_BPS_PER_SIDE = 1.0
FALLBACK_SPREAD_BPS = 2.0      # used only where no book reading exists; flagged

TALIB_PROJECTION = [
    "NATR_14_outReal", "NATR_50_outReal", "NATR_200_outReal",
    "ROCP_5_outReal", "ROCP_14_outReal", "ROCP_50_outReal", "ROCP_200_outReal",
    "RSI_14_outReal", "RSI_50_outReal", "CCI_20_outReal", "CMO_14_outReal",
    "WILLR_14_outReal", "MFI_14_outReal", "ULTOSC_outReal", "STOCH_SMA_outSlowK",
    "BOP_outReal", "AROONOSC_14_outReal", "AROONOSC_50_outReal", "IMI_14_outReal",
    "ADX_14_outReal", "ADX_50_outReal", "PLUS_DI_14_outReal", "MINUS_DI_14_outReal",
    "PPO_EMA_outReal", "TRIX_14_outReal", "TRIX_50_outReal",
    "HT_TRENDMODE_outInteger", "HT_DCPERIOD_outReal", "HT_DCPHASE_outReal",
]
DERIVED = ["dist_ema20_atr", "dist_ema50_atr", "dist_ema200_atr",
           "don_pos_20", "don_pos_50", "don_pos_200",
           "natr_ratio_14_200", "di_diff_14", "macd_hist_atr",
           "vol_ratio_5_50", "vol_ratio_50_200", "stddev5_atr",
           "slope14_atr", "slope50_atr", "ema_stack", "bbw_20"]

# Macro columns that are levels or publication ages: within months of
# history they identify the date, not the state (same list as recipe-1).
MACRO_EXCLUDED = {"us2y_level", "us10y_level", "us10y_real_level", "curve_2s10s",
                  "breakeven_10y", "vix_level", "fixing_age_hours", "usdkrw_age_hours",
                  "equity_age_hours", "rates_age_hours"}


def ratio(a, b):
    """recipe-1 ratio(): NaN unless both finite and the denominator positive."""
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    out = np.full(np.broadcast(a, b).shape, np.nan)
    ok = np.isfinite(a) & np.isfinite(b) & (b > 0)
    np.divide(a, b, out=out, where=ok)
    return out


def derived_block(t):
    """recipe-1 DerivedRecipe::compute, column for column, plus bbw_20.

    `t` maps talib-core names to arrays (invalid = NaN)."""
    close = 2.0 * t["WCLPRICE_outReal"] - t["MEDPRICE_outReal"]
    atr = t["ATR_14_outReal"]
    out = {}
    for n, e in (("20", "EMA_20_outReal"), ("50", "EMA_50_outReal"), ("200", "EMA_200_outReal")):
        out["dist_ema%s_atr" % n] = ratio(close - t[e], atr)
    for n in ("20", "50", "200"):
        hi, lo = t["MAX_HIGH_%s_outReal" % n], t["MIN_LOW_%s_outReal" % n]
        out["don_pos_%s" % n] = ratio(close - lo, hi - lo)
    out["natr_ratio_14_200"] = ratio(t["NATR_14_outReal"], t["NATR_200_outReal"])
    d = t["PLUS_DI_14_outReal"] - t["MINUS_DI_14_outReal"]
    out["di_diff_14"] = np.where(np.isfinite(d), d, np.nan)
    out["macd_hist_atr"] = ratio(t["MACD_outMACDHist"], atr)
    out["vol_ratio_5_50"] = ratio(t["SMA_VOL_5_outReal"], t["SMA_VOL_50_outReal"])
    out["vol_ratio_50_200"] = ratio(t["SMA_VOL_50_outReal"], t["SMA_VOL_200_outReal"])
    out["stddev5_atr"] = ratio(t["STDDEV_5_outReal"], atr)
    out["slope14_atr"] = ratio(t["LINEARREG_SLOPE_14_outReal"], atr)
    out["slope50_atr"] = ratio(t["LINEARREG_SLOPE_50_outReal"], atr)
    e20, e50, e200 = t["EMA_20_outReal"], t["EMA_50_outReal"], t["EMA_200_outReal"]
    fin = np.isfinite(close) & np.isfinite(e20) & np.isfinite(e50) & np.isfinite(e200)
    stack = np.where((close > e20) & (e20 > e50) & (e50 > e200), 1.0,
                     np.where((close < e20) & (e20 < e50) & (e50 < e200), -1.0, 0.0))
    out["ema_stack"] = np.where(fin, stack, np.nan)
    out["bbw_20"] = ratio(t["BBANDS_20_SMA_outRealUpperBand"] - t["BBANDS_20_SMA_outRealLowerBand"],
                          t["BBANDS_20_SMA_outRealMiddleBand"])
    return out
