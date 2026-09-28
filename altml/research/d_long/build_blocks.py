"""Stage 1: D_long_v1 blocks.

Rows: (instrument, decision time tau) on the phased 15-minute grid where the
Bitget 1m bar closing at t = tau - 5 s is present, ok, and its reconstructed
talib-core vector is VALID. Every block below is aligned to those rows and
saved as float32 .npy with its column list; labels live in their own block and
are never joined into a feature block.

Blocks
  rows       instrument, t_close_ms, tau_ms, phase
  tl_<tf>    Bitget talib-core projection + recipe-1 derived, bar available at tau
             (close + p90 lag <= tau, age <= freshness), one block per 1m..1d
  age        age of each aligned Bitget bar at tau (seconds), per tf
  xtf        cross-timeframe relations (RSI gaps, ATR ratios, momentum ratios)
  cdl        candle-derived Bitget state: returns, realised vol, VWAP distances,
             taker imbalance, volume anomaly (all from minutes <= t)
  xv         cross-venue: deviation from the other venues' consensus, consensus
             returns, dispersion, short lead
  ref_<tf>   reference-venue talib (median of Binance/OKX/Bybit) at 15m/1h/4h
  drv        derivatives (from 2026-06-27): funding, basis, open-interest flow
  mac        macro-core (from 2026-05-27), levels/ages excluded
  mic        Bitget book (from 2026-09-01): spread, depth, imbalance, microprice
  cost       cost model v2 inputs known at tau (fee, observed spread, source flag)
  setup      setup/event descriptors (continuous geometry + detector-style flags)
  cand       real AltStrategy candidates (2026-09-11..): any / per family / side
  lab        LABELS: per horizon and entry latency, log return; MFE/MAE and
             time-to at latency 0; realised exit spread where observed
"""

import json
import os
import sys

import numpy as np
import pandas as pd

import common as C
from split_raw import N_MIN, START_MS

TFS = ["1m", "5m", "15m", "30m", "1h", "4h", "1d"]
REF_TFS = ["15m", "1h", "4h"]
REF_COLS = ["RSI_14_outReal", "ROCP_14_outReal", "NATR_14_outReal", "ADX_14_outReal"]


def load_talib(root, ex, tf):
    hdr = open(os.path.join(root, "talib", "header.csv")).read().strip().split(",")
    f = os.path.join(root, "talib", "ex%d_%s.csv.gz" % (ex, tf))
    dt = {h: np.float32 for h in hdr[4:]}
    dt.update({"instrument_id": np.int32, "open_time_ms": np.int64, "ok": np.int8, "status": str})
    df = pd.read_csv(f, header=None, names=hdr, dtype=dt)
    return df


def align(avail_ms, t_query, max_age_ms, close_ms):
    """Index of the latest bar with avail <= t (and age <= max), else -1."""
    i = np.searchsorted(avail_ms, t_query, side="right") - 1
    ok = i >= 0
    age = np.where(ok, t_query - close_ms[np.clip(i, 0, None)], np.inf)
    if max_age_ms > 0:
        ok &= age <= max_age_ms + C.AVAIL_LAG_MS_MAX
    return np.where(ok, i, -1), age


C.AVAIL_LAG_MS_MAX = max(C.AVAIL_LAG_MS.values())


def save(out, name, cols, arr):
    np.save(os.path.join(out, name + ".npy"), np.ascontiguousarray(arr, dtype=np.float32))
    with open(os.path.join(out, name + ".json"), "w") as f:
        json.dump(cols, f)


def minute_index(t_ms):
    return ((t_ms - START_MS) // C.MIN_MS).astype(np.int64)


def main(root, m1dir, out):
    os.makedirs(out, exist_ok=True)
    ids = sorted(int(f.split("_")[2].split(".")[0]) for f in os.listdir(m1dir) if f.startswith("m1_ex1_"))

    # ---------------------------------------------------------------- rows
    tl1 = load_talib(root, C.BITGET, "1m")
    rows = []
    for iid in ids:
        g = tl1[(tl1.instrument_id == iid) & (tl1.status == "VALID")]
        t_close = g.open_time_ms.values + C.MIN_MS
        phase = ((t_close // C.GRID_MS) + iid) % C.PHASES
        sel = (phase == 0) & (t_close < C.CUTOFF_MS)
        rows.append(pd.DataFrame({"instrument_id": iid, "t_close_ms": t_close[sel],
                                  "tau_ms": t_close[sel] + C.DECISION_DELAY_MS}))
    R = pd.concat(rows, ignore_index=True).sort_values(["instrument_id", "t_close_ms"]).reset_index(drop=True)
    del tl1
    n = len(R)
    save(out, "rows", ["instrument_id", "t_close_ms", "tau_ms"],
         R[["instrument_id", "t_close_ms", "tau_ms"]].values.astype(np.float64))
    np.save(os.path.join(out, "rows_i64.npy"), R[["instrument_id", "t_close_ms", "tau_ms"]].values.astype(np.int64))
    print("rows", n, flush=True)
    inst = R.instrument_id.values
    tau = R.tau_ms.values
    tclose = R.t_close_ms.values
    bounds = {iid: (np.searchsorted(inst, iid, "left"), np.searchsorted(inst, iid, "right")) for iid in ids}

    # ---------------------------------------------------------- talib blocks
    ages = np.full((n, len(TFS)), np.nan, dtype=np.float32)
    tl_blocks = {}
    for k, tf in enumerate(TFS):
        df = load_talib(root, C.BITGET, tf)
        cols = C.TALIB_PROJECTION + C.DERIVED
        blk = np.full((n, len(cols)), np.nan, dtype=np.float32)
        for iid in ids:
            a, b = bounds[iid]
            if a == b:
                continue
            g = df[(df.instrument_id == iid) & (df.status == "VALID")]
            if g.empty:
                continue
            close_ms = g.open_time_ms.values + C.SPAN_MS[tf]
            avail = close_ms + C.AVAIL_LAG_MS[tf]
            if tf == "1m":
                pos = np.searchsorted(close_ms, tclose[a:b])
                ok = (pos < len(close_ms)) & (close_ms[np.clip(pos, 0, len(close_ms) - 1)] == tclose[a:b])
                idx = np.where(ok, pos, -1)
                age = np.where(ok, tau[a:b] - close_ms[np.clip(pos, 0, None)], np.nan)
            else:
                i = np.searchsorted(avail, tau[a:b], side="right") - 1
                ic = np.clip(i, 0, None)
                age = np.where(i >= 0, tau[a:b] - close_ms[ic], np.nan)
                idx = np.where((i >= 0) & (age <= C.FRESH_MS[tf] + C.AVAIL_LAG_MS[tf]), i, -1)
            t = {c: g[c].values.astype(np.float64) for c in g.columns[4:]}
            d = C.derived_block(t)
            vals = np.column_stack([t[c] for c in C.TALIB_PROJECTION] + [d[c] for c in C.DERIVED])
            take = idx >= 0
            blk[np.arange(a, b)[take]] = vals[idx[take]]
            ages[np.arange(a, b)[take], k] = age[take] / 1000.0
        save(out, "tl_" + tf, [tf + ":" + c for c in cols], blk)
        tl_blocks[tf] = blk
        del df
        print("talib", tf, "coverage", float(np.isfinite(blk[:, 7]).mean()), flush=True)
    save(out, "age", ["age_s_" + tf for tf in TFS], ages)

    # ------------------------------------------------------ cross-timeframe
    col = {c: i for i, c in enumerate(C.TALIB_PROJECTION + C.DERIVED)}
    def g(tf, c):
        return tl_blocks[tf][:, col[c]].astype(np.float64)
    xtf_cols, xtf = [], []
    for a_, b_ in (("1m", "15m"), ("5m", "1h"), ("15m", "1h"), ("1h", "4h"), ("4h", "1d")):
        xtf_cols.append("rsi14_%s_minus_%s" % (a_, b_)); xtf.append(g(a_, "RSI_14_outReal") - g(b_, "RSI_14_outReal"))
    for a_, b_ in (("1m", "1h"), ("15m", "4h"), ("1h", "1d")):
        xtf_cols.append("natr14_%s_over_%s" % (a_, b_)); xtf.append(C.ratio(g(a_, "NATR_14_outReal"), g(b_, "NATR_14_outReal")))
    for a_, b_ in (("5m", "1h"), ("15m", "4h"), ("1h", "1d")):
        xtf_cols.append("rocp14_%s_minus_%s" % (a_, b_)); xtf.append(g(a_, "ROCP_14_outReal") - g(b_, "ROCP_14_outReal"))
    for a_, b_ in (("15m", "1h"), ("1h", "4h")):
        xtf_cols.append("ema_stack_%s_x_%s" % (a_, b_)); xtf.append(g(a_, "ema_stack") * g(b_, "ema_stack"))
    save(out, "xtf", xtf_cols, np.column_stack(xtf))
    del tl_blocks

    # ------------------------------------- candle-derived, cross-venue, labels
    cdl_cols = ["ret_5m", "ret_15m", "ret_1h", "ret_4h", "ret_24h", "rv_1h", "rv_4h", "rv_24h",
                "rv_ratio_1h_24h", "dist_vwap_sess_bps", "dist_vwap_1h_bps", "dist_vwap_4h_bps",
                "dist_vwap_24h_bps", "taker_imb_15m", "taker_imb_1h", "taker_imb_4h",
                "cvd_1h_over_vol", "qv_anom_15m", "qv_anom_1h", "trades_anom_15m",
                "range_pos_24h", "gap_minutes_24h"]
    xv_cols = ["xv_dev_bps", "xv_cons_ret_15m", "xv_cons_ret_1h", "xv_bg_minus_cons_15m",
               "xv_bg_minus_cons_1h", "xv_disp_15m", "xv_lead_bn_5m", "xv_lead_cons_5m",
               "xv_venues"]
    lab_cols = []
    for h in C.HORIZONS:
        for L in C.LATENCIES:
            lab_cols.append("r_%s_L%d" % (h, L))
        lab_cols += ["mfe_%s" % h, "mae_%s" % h, "tmfe_%s" % h, "tmae_%s" % h]
    cdl = np.full((n, len(cdl_cols)), np.nan, dtype=np.float32)
    xv = np.full((n, len(xv_cols)), np.nan, dtype=np.float32)
    lab = np.full((n, len(lab_cols)), np.nan, dtype=np.float32)
    for iid in ids:
        a, b = bounds[iid]
        if a == b:
            continue
        m = dict(np.load(os.path.join(m1dir, "m1_ex1_%d.npz" % iid)))
        c, h, l = m["c"], m["h"], m["l"]
        qv, bv, tb, ts, nt = (m[k].astype(np.float64) for k in ("qv", "bv", "tb", "ts", "n"))
        mi = minute_index(tclose[a:b]) - 1           # index of the bar that closes at t
        lc = np.log(c)
        def back(k):
            j = mi - k
            return np.where(j >= 0, lc[np.clip(j, 0, None)], np.nan)
        x = {}
        for name, k in (("ret_5m", 5), ("ret_15m", 15), ("ret_1h", 60), ("ret_4h", 240), ("ret_24h", 1440)):
            x[name] = lc[mi] - back(k)
        r1 = np.diff(lc, prepend=np.nan)
        s = pd.Series(r1)
        for name, w in (("rv_1h", 60), ("rv_4h", 240), ("rv_24h", 1440)):
            x[name] = s.rolling(w, min_periods=int(w * 0.8)).std().values[mi]
        x["rv_ratio_1h_24h"] = C.ratio(x["rv_1h"], x["rv_24h"])
        pv = pd.Series(np.nan_to_num(qv)); vv = pd.Series(np.nan_to_num(bv))
        for name, w in (("dist_vwap_1h_bps", 60), ("dist_vwap_4h_bps", 240), ("dist_vwap_24h_bps", 1440)):
            vw = (pv.rolling(w).sum() / vv.rolling(w).sum()).values
            x[name] = (c[mi] / vw[mi] - 1.0) * 1e4
        day = (np.arange(N_MIN) * C.MIN_MS + START_MS) // 86_400_000
        cq = pd.Series(np.nan_to_num(qv)).groupby(day).cumsum().values
        cb = pd.Series(np.nan_to_num(bv)).groupby(day).cumsum().values
        x["dist_vwap_sess_bps"] = (c[mi] / C.ratio(cq[mi], cb[mi]) - 1.0) * 1e4
        tb_s, ts_s = pd.Series(np.nan_to_num(tb)), pd.Series(np.nan_to_num(ts))
        for name, w in (("taker_imb_15m", 15), ("taker_imb_1h", 60), ("taker_imb_4h", 240)):
            B, S = tb_s.rolling(w).sum().values, ts_s.rolling(w).sum().values
            tot = B[mi] + S[mi]
            x[name] = (B[mi] - S[mi]) / np.where(tot > 0, tot, np.nan)
        delta = (tb_s - ts_s).rolling(60).sum().values
        x["cvd_1h_over_vol"] = delta[mi] / np.where(vv.rolling(60).sum().values[mi] > 0, vv.rolling(60).sum().values[mi], np.nan)
        # volume anomaly: last window against the same window at the same time
        # of day over the previous 7 days (all strictly in the past)
        for name, w in (("qv_anom_15m", 15), ("qv_anom_1h", 60)):
            cur = pv.rolling(w).sum().values
            prev = np.column_stack([np.where(mi - 1440 * d >= 0, cur[np.clip(mi - 1440 * d, 0, None)], np.nan) for d in range(1, 8)])
            base = np.nanmean(np.where(prev > 0, prev, np.nan), axis=1)
            x[name] = np.log(C.ratio(cur[mi], base))
        nt_s = pd.Series(np.nan_to_num(nt)); curn = nt_s.rolling(15).sum().values
        prevn = np.column_stack([np.where(mi - 1440 * d >= 0, curn[np.clip(mi - 1440 * d, 0, None)], np.nan) for d in range(1, 8)])
        x["trades_anom_15m"] = np.log(C.ratio(curn[mi], np.nanmean(np.where(prevn > 0, prevn, np.nan), axis=1)))
        hh = pd.Series(h).rolling(1440, min_periods=1200).max().values
        ll = pd.Series(l).rolling(1440, min_periods=1200).min().values
        x["range_pos_24h"] = C.ratio(c[mi] - ll[mi], hh[mi] - ll[mi])
        gaps = pd.Series((~m["present"]).astype(np.float64)).rolling(1440).sum().values
        x["gap_minutes_24h"] = gaps[mi]
        cdl[a:b] = np.column_stack([x[k] for k in cdl_cols])

        # cross-venue
        others = []
        for ex in (C.BINANCE, C.OKX, C.BYBIT):
            f = os.path.join(m1dir, "m1_ex%d_%d.npz" % (ex, iid))
            others.append(np.load(f)["c"] if os.path.exists(f) else np.full(N_MIN, np.nan))
        oc = np.column_stack(others)
        cons = np.nanmedian(oc, axis=1)
        y = {}
        y["xv_dev_bps"] = (c[mi] / cons[mi] - 1.0) * 1e4
        lo_ = np.log(oc)
        def oret(k):
            j = np.clip(mi - k, 0, None)
            return np.where((mi - k)[:, None] >= 0, lo_[mi] - lo_[j], np.nan)
        for name, k in (("15m", 15), ("1h", 60)):
            rr = oret(k)
            y["xv_cons_ret_" + name] = np.nanmedian(rr, axis=1)
            y["xv_bg_minus_cons_" + name] = x["ret_" + name] - y["xv_cons_ret_" + name]
            if name == "15m":
                y["xv_disp_15m"] = np.nanstd(np.column_stack([rr, x["ret_15m"]]), axis=1)
        r5 = oret(5)
        y["xv_lead_bn_5m"] = r5[:, 0] - x["ret_5m"]
        y["xv_lead_cons_5m"] = np.nanmedian(r5, axis=1) - x["ret_5m"]
        y["xv_venues"] = np.isfinite(oc[mi]).sum(axis=1).astype(np.float64)
        xv[a:b] = np.column_stack([y[k] for k in xv_cols])

        # labels (outcomes; never features)
        z = {}
        for hname, hm in C.HORIZONS.items():
            for L in C.LATENCIES:
                e = mi + L
                x_ = e + hm
                okr = x_ < N_MIN
                ec, xc = c[np.clip(e, 0, N_MIN - 1)], c[np.clip(x_, 0, N_MIN - 1)]
                z["r_%s_L%d" % (hname, L)] = np.where(okr, np.log(xc / ec), np.nan)
            # path from the close at t: bars t+1 .. t+h (the decision bar excluded)
            win = hm
            fwd_h = pd.Series(h[::-1]).rolling(win, min_periods=win).max().values[::-1]
            fwd_l = pd.Series(l[::-1]).rolling(win, min_periods=win).min().values[::-1]
            j = mi + 1
            okp = (j + win - 1) < N_MIN
            jc = np.clip(j, 0, N_MIN - 1)
            z["mfe_%s" % hname] = np.where(okp, np.log(fwd_h[jc] / c[mi]), np.nan)
            z["mae_%s" % hname] = np.where(okp, np.log(fwd_l[jc] / c[mi]), np.nan)
            # time to extreme (minutes), argmax over the forward window
            hw = np.lib.stride_tricks.sliding_window_view(np.nan_to_num(h, nan=-np.inf), win)
            lw = np.lib.stride_tricks.sliding_window_view(np.nan_to_num(l, nan=np.inf), win)
            jj = np.clip(j, 0, hw.shape[0] - 1)
            z["tmfe_%s" % hname] = np.where(okp, hw[jj].argmax(axis=1) + 1, np.nan)
            z["tmae_%s" % hname] = np.where(okp, lw[jj].argmin(axis=1) + 1, np.nan)
            # a path through a missing minute is not a path
            miss = pd.Series((~m["present"]).astype(np.float64)[::-1]).rolling(win, min_periods=1).sum().values[::-1]
            bad = ~okp | (miss[jc] > 0)
            for k in ("mfe_%s" % hname, "mae_%s" % hname, "tmfe_%s" % hname, "tmae_%s" % hname):
                z[k] = np.where(bad, np.nan, z[k])
            for L in C.LATENCIES:
                e = mi + L
                bad_r = (e + hm >= N_MIN) | ~m["present"][np.clip(e, 0, N_MIN - 1)] | ~m["present"][np.clip(e + hm, 0, N_MIN - 1)]
                z["r_%s_L%d" % (hname, L)] = np.where(bad_r, np.nan, z["r_%s_L%d" % (hname, L)])
        lab[a:b] = np.column_stack([z[k] for k in lab_cols])
        print("instrument", iid, "done", flush=True)
    save(out, "cdl", cdl_cols, cdl)
    save(out, "xv", xv_cols, xv)
    save(out, "lab", lab_cols, lab)

    # ------------------------------------------------- reference talib blocks
    for tf in REF_TFS:
        vals = []
        for ex in (C.BINANCE, C.OKX, C.BYBIT):
            df = load_talib(root, ex, tf)
            blk = np.full((n, len(REF_COLS)), np.nan, dtype=np.float64)
            for iid in ids:
                a, b = bounds[iid]
                g_ = df[(df.instrument_id == iid) & (df.status == "VALID")]
                if a == b or g_.empty:
                    continue
                close_ms = g_.open_time_ms.values + C.SPAN_MS[tf]
                i = np.searchsorted(close_ms + C.AVAIL_LAG_MS[tf], tau[a:b], side="right") - 1
                ic = np.clip(i, 0, None)
                age = tau[a:b] - close_ms[ic]
                take = (i >= 0) & (age <= C.FRESH_MS[tf] + C.AVAIL_LAG_MS[tf])
                blk[np.arange(a, b)[take]] = g_[REF_COLS].values[ic[take]]
            vals.append(blk)
            del df
        med = np.nanmedian(np.stack(vals), axis=0)
        save(out, "ref_" + tf, ["ref_%s:%s" % (tf, c) for c in REF_COLS], med)
        print("ref", tf, flush=True)

    # ------------------------------------------------------------ derivatives
    # read in chunks and keep, per (venue, instrument), only what a block uses
    need = {C.BITGET: ["mark", "index", "fund", "bid", "ask"], C.BINANCE: ["fund"],
            C.OKX: ["fund", "oi"], C.BYBIT: ["fund", "oi"]}
    parts = {}
    for ch_ in pd.read_csv(os.path.join(root, "raw", "derivatives_1m.csv.gz"), header=None,
                           names=["ex", "id", "t", "mark", "index", "fund", "oi", "bid", "ask"],
                           chunksize=2_000_000):
        for (ex, iid), g_ in ch_.groupby(["ex", "id"]):
            cols_ = ["t"] + need[int(ex)]
            parts.setdefault((int(ex), int(iid)), []).append(g_[cols_].astype({"t": np.int64}).astype(
                {c_: np.float32 for c_ in need[int(ex)]}))
    d = {k: pd.concat(v, ignore_index=True) for k, v in parts.items()}
    del parts
    drv_cols = ["fund_bg", "fund_bg_z7d", "fund_cons", "basis_bg_bps", "oi_chg_1h", "oi_chg_4h",
                "oi_chg_24h", "spread_obs_bps"]
    drv = np.full((n, len(drv_cols)), np.nan, dtype=np.float32)
    spread_obs = np.full(n, np.nan)
    for iid in ids:
        a, b = bounds[iid]
        if a == b:
            continue
        q = {}
        for ex in C.VENUES:
            g_ = d.get((ex, iid))
            cols_ = ["mark", "index", "fund", "oi", "bid", "ask"]
            if g_ is not None and not g_.empty:
                k = ((g_.t.values - START_MS) // C.MIN_MS).astype(np.int64)
                keep = (k >= 0) & (k < N_MIN)
                s_ = g_.iloc[keep].drop(columns="t").set_index(k[keep])
                s_ = s_[~s_.index.duplicated()].reindex(np.arange(N_MIN))
                for c_ in cols_:
                    if c_ not in s_:
                        s_[c_] = np.nan
            else:
                s_ = pd.DataFrame(np.nan, index=np.arange(N_MIN), columns=cols_)
            q[ex] = s_.astype(np.float64)
        mi = minute_index(tclose[a:b]) - 1
        bg = q[C.BITGET]
        fund = bg.fund.ffill(limit=60).values
        f7 = bg.fund.rolling(7 * 1440, min_periods=1440)
        fz = ((bg.fund - f7.mean()) / f7.std()).ffill(limit=60).values
        fc = pd.concat([q[e].fund for e in C.VENUES], axis=1).median(axis=1).ffill(limit=60).values
        basis = ((bg.mark / bg["index"] - 1.0) * 1e4).ffill(limit=5).values
        oi = (q[C.OKX].oi.ffill(limit=5) + q[C.BYBIT].oi.ffill(limit=5)).values
        loi = np.log(np.where(oi > 0, oi, np.nan))
        def ch(k):
            j = mi - k
            return np.where(j >= 0, loi[mi] - loi[np.clip(j, 0, None)], np.nan)
        # observed relative spread: trailing 60-minute median of the book
        # readings, strictly up to the decision bar
        rel = ((bg.ask - bg.bid) / ((bg.ask + bg.bid) / 2.0) * 1e4)
        rel = rel.where((rel > 0) & (rel < 200))
        sp = rel.rolling(60, min_periods=3).median().values
        drv[a:b] = np.column_stack([fund[mi], fz[mi], fc[mi], basis[mi], ch(60), ch(240), ch(1440), sp[mi]])
        spread_obs[a:b] = sp[mi]
    save(out, "drv", drv_cols, drv)
    del d
    print("derivatives", flush=True)

    # ------------------------------------------------------------ microstructure
    bk = pd.read_csv(os.path.join(root, "raw", "book_1m.csv.gz"), header=None,
                     names=["ex", "id", "t", "bid", "ask", "rel", "bd1", "ad1", "bd5", "ad5", "bd10",
                            "ad10", "imb1", "imb5", "imb10", "micro", "snaps"])
    bk = bk[bk.ex == C.BITGET]
    mic_cols = ["mic_rel_spread_bps", "mic_depth5_log", "mic_depth10_log", "mic_imb1", "mic_imb5",
                "mic_imb10", "mic_microprice_dev_bps", "mic_imb5_mean15"]
    mic = np.full((n, len(mic_cols)), np.nan, dtype=np.float32)
    for iid in ids:
        a, b = bounds[iid]
        g_ = bk[bk.id == iid]
        if a == b or g_.empty:
            continue
        k = ((g_.t.values - START_MS) // C.MIN_MS).astype(np.int64)
        s_ = g_.set_index(k)
        s_ = s_[~s_.index.duplicated()].reindex(np.arange(N_MIN))
        mi = minute_index(tclose[a:b]) - 1
        mid = (s_.bid + s_.ask) / 2.0
        mic[a:b] = np.column_stack([
            (s_.rel * 1e4).values[mi] if s_.rel.max() < 1 else s_.rel.values[mi],
            np.log(s_.bd5 + s_.ad5).values[mi], np.log(s_.bd10 + s_.ad10).values[mi],
            s_.imb1.values[mi], s_.imb5.values[mi], s_.imb10.values[mi],
            ((s_.micro / mid - 1.0) * 1e4).values[mi], s_.imb5.rolling(15, min_periods=10).mean().values[mi]])
    save(out, "mic", mic_cols, mic)
    # the book is the best spread reading where it exists
    book_sp = mic[:, 0].astype(np.float64)
    spread_obs = np.where(np.isfinite(book_sp) & (book_sp > 0), book_sp, spread_obs)
    del bk
    print("micro", flush=True)

    # ------------------------------------------------------------------ cost v2
    src = np.where(np.isfinite(spread_obs), 1.0, 0.0)
    spread_used = np.where(np.isfinite(spread_obs), spread_obs, C.FALLBACK_SPREAD_BPS)
    cost_rt = 2 * C.FEE_BPS_PER_SIDE + spread_used + 2 * C.IMPACT_BPS_PER_SIDE
    save(out, "cost", ["fee_rt_bps", "spread_bps", "impact_rt_bps", "cost_rt_bps", "spread_observed"],
         np.column_stack([np.full(n, 2 * C.FEE_BPS_PER_SIDE), spread_used,
                          np.full(n, 2 * C.IMPACT_BPS_PER_SIDE), cost_rt, src]))

    # ------------------------------------------------------------------- macro
    mc = [r.split(",")[1] for r in open(os.path.join(root, "raw", "macro_columns.csv")).read().split()]
    keep = [i for i, name in enumerate(mc) if name not in C.MACRO_EXCLUDED]
    mv = pd.read_csv(os.path.join(root, "raw", "macro_1m.csv.gz"), header=None,
                     names=["t", "computed", "method", "status", "values", "bmp"])
    mv = mv[mv.status != "WARMING"]
    vals = np.array([np.array(v.strip("{}").split(","), dtype=np.float64) for v in mv["values"]])
    bits = np.array([np.unpackbits(np.frombuffer(bytes.fromhex(x), dtype=np.uint8), bitorder="little")[:len(mc)]
                     for x in mv["bmp"]], dtype=bool)
    vals = np.where(bits, vals, np.nan)[:, keep]
    mt = mv.t.values + C.MIN_MS                   # a macro minute is known at its close
    i = np.searchsorted(mt, tau, side="right") - 1
    ok = (i >= 0) & (tau - mt[np.clip(i, 0, None)] <= 5 * C.MIN_MS)
    mac = np.full((n, len(keep)), np.nan, dtype=np.float32)
    mac[ok] = vals[i[ok]]
    save(out, "mac", ["mac:" + mc[k] for k in keep], mac)
    del mv, vals, bits
    print("macro", flush=True)

    # ------------------------------------------------------ real candidates
    cd = pd.read_csv(os.path.join(root, "raw", "altstrategy_candidates.csv.gz"))
    fams = sorted(cd.strategy_family.dropna().unique())
    cand_cols = ["cand_any", "cand_long", "cand_short"] + ["cand_fam:" + f for f in fams]
    cand = np.zeros((n, len(cand_cols)), dtype=np.float32)
    first = cd.setup_time_ms.min()
    for iid in ids:
        a, b = bounds[iid]
        g_ = cd[cd.canonical_pair_id == iid].sort_values("setup_time_ms")
        if a == b:
            continue
        lo = np.searchsorted(g_.setup_time_ms.values, tau[a:b] - 15 * C.MIN_MS, "right")
        hi = np.searchsorted(g_.setup_time_ms.values, tau[a:b], "right")
        side = g_.side.values; fam = g_.strategy_family.values
        for r in range(b - a):
            if hi[r] > lo[r]:
                cand[a + r, 0] = 1
                ss = side[lo[r]:hi[r]]; ff = fam[lo[r]:hi[r]]
                cand[a + r, 1] = float((ss == "LONG").any())
                cand[a + r, 2] = float((ss == "SHORT").any())
                for fi, f in enumerate(fams):
                    cand[a + r, 3 + fi] = float((ff == f).any())
    cand[tau < first] = np.nan                   # before AltStrategy recorded: unknown, not zero
    save(out, "cand", cand_cols, cand)

    manifest = {"dataset": C.DATASET_VERSION, "rows": int(n), "cutoff_ms": C.CUTOFF_MS,
                "decision_delay_ms": C.DECISION_DELAY_MS, "avail_lag_ms": C.AVAIL_LAG_MS,
                "fresh_ms": C.FRESH_MS, "latencies_min": C.LATENCIES, "horizons": C.HORIZONS,
                "cost_model": {"version": "cost-v2", "fee_bps_per_side": C.FEE_BPS_PER_SIDE,
                               "impact_bps_per_side": C.IMPACT_BPS_PER_SIDE,
                               "fallback_spread_bps": C.FALLBACK_SPREAD_BPS}}
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print("done", flush=True)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
