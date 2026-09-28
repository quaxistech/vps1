"""D_long_v1 experiment harness: one protocol for every comparison.

Protocol (fixed before any result was seen, identical for every variant):
  folds      test months 2026-05 .. 2026-09 (expanding window); training is
             everything earlier, minus a purge of horizon + max latency, minus
             the validation slice
  validation the 14 days before the purge: early stopping, class-conditional
             means, and the policy margin (nested: chosen here, applied to test)
  model      LightGBM, fixed parameters below, 2 threads, deterministic
  entry      latency 1 minute (the ML path) unless stated; 0/3/7 reported
  economics  one position per instrument at a time; per trade: gross, latency
             effect, fee, spread, impact, net; day-clustered t of net; the
             fold's unconditional drift and a long-share-matched benchmark
"""

import json
import os
import sys
import time

import lightgbm as lgb
import numpy as np
import pandas as pd

import common as C

DS = os.environ.get("DLONG", "")
PARAMS = dict(learning_rate=0.05, num_leaves=15, min_data_in_leaf=500, feature_fraction=0.7,
              bagging_fraction=0.7, bagging_freq=1, lambda_l2=10.0, max_depth=6, max_bin=255,
              num_threads=2, seed=42, deterministic=True, verbosity=-1, force_col_wise=True)
MAX_ROUNDS, EARLY = 800, 50
VAL_DAYS = 14
MARGIN_GRID = (0.0, 2.0, 5.0, 10.0, 20.0)
TEST_MONTHS = ("2026-05", "2026-06", "2026-07", "2026-08", "2026-09")


# ------------------------------------------------------------------ data
_cache = {}
# (variant, fold, horizon) already written to results.jsonl: a resumed run skips
# them. Variants that keep predictions for later analysis always recompute.
DONE = set()
if DS and os.path.exists(os.path.join(DS, "results.jsonl")):
    for _line in open(os.path.join(DS, "results.jsonl")):
        try:
            _r = json.loads(_line)
            DONE.add((_r["variant"], _r["fold"], _r.get("horizon")))
        except (ValueError, KeyError):
            pass


def block(name):
    if name not in _cache:
        arr = np.load(os.path.join(DS, name + ".npy"), mmap_mode="r")
        cols = json.load(open(os.path.join(DS, name + ".json")))
        _cache[name] = (arr, cols)
    return _cache[name]


def rows():
    r = np.load(os.path.join(DS, "rows_i64.npy"))
    return pd.DataFrame(r, columns=["instrument_id", "t_close_ms", "tau_ms"])


def label(name):
    arr, cols = block("lab")
    return np.asarray(arr[:, cols.index(name)], dtype=np.float64)


def cost_col(name):
    arr, cols = block("cost")
    return np.asarray(arr[:, cols.index(name)], dtype=np.float64)


def features(blocks, idx=None, extra=None):
    mats, names = [], []
    for b in blocks:
        arr, cols = block(b)
        mats.append(np.asarray(arr if idx is None else arr[idx], dtype=np.float32))
        names += cols
    if extra is not None:
        for n_, v in extra.items():
            mats.append((v if idx is None else v[idx]).astype(np.float32)[:, None])
            names.append(n_)
    return np.hstack(mats), names


# ------------------------------------------------------------ setup descriptors
def setup_descriptors():
    """Detector-style descriptors from point-in-time blocks: continuous geometry
    plus the flag a rule-based detector would raise. PROXIES of the 16 AltStrategy
    detectors, not the detectors themselves (those are only recorded from
    2026-09-11: see the `cand` block)."""
    def g(b, c):
        arr, cols = block(b)
        return np.asarray(arr[:, cols.index(c)], dtype=np.float64)
    d = {}
    don15 = g("tl_15m", "15m:don_pos_20")
    d["su_breakout_up_15m"] = (don15 > 0.95).astype(float)
    d["su_breakout_dn_15m"] = (don15 < 0.05).astype(float)
    d["su_compression_15m"] = (g("tl_15m", "15m:natr_ratio_14_200") < 0.7).astype(float)
    st1h = g("tl_1h", "1h:ema_stack")
    de15 = g("tl_15m", "15m:dist_ema20_atr")
    d["su_pullback_long"] = ((st1h == 1) & (de15 < 0) & (de15 > -3)).astype(float)
    d["su_pullback_short"] = ((st1h == -1) & (de15 > 0) & (de15 < 3)).astype(float)
    qa = g("cdl", "qv_anom_15m"); r15 = g("cdl", "ret_15m"); rv = g("cdl", "rv_1h")
    d["su_momentum_burst"] = ((qa > np.log(2)) & (np.abs(r15) > 2 * rv * np.sqrt(15))).astype(float)
    ti = g("cdl", "taker_imb_15m")
    d["su_cvd_absorption"] = ((ti * r15 < 0) & (np.abs(ti) > 0.2)).astype(float)
    rp = g("cdl", "range_pos_24h"); adx = g("tl_1h", "1h:ADX_14_outReal")
    d["su_range_extreme"] = (((rp > 0.95) | (rp < 0.05)) & (adx < 20)).astype(float)
    dev = g("xv", "xv_dev_bps")
    d["su_dislocation"] = (np.abs(dev) > 10).astype(float)
    fz = g("drv", "fund_bg_z7d"); oi = g("drv", "oi_chg_4h")
    d["su_crowding"] = (((np.abs(fz) > 2) & (oi > 0.02))).astype(float)
    vw = g("cdl", "dist_vwap_sess_bps")
    d["su_vwap_near"] = (np.abs(vw) < 10).astype(float)
    flags = [k for k in d]
    anyf = np.zeros_like(don15)
    for k in flags:
        anyf = np.maximum(anyf, np.nan_to_num(d[k]))
    d["su_any"] = anyf
    d["su_count"] = sum(np.nan_to_num(d[k]) for k in flags)
    return d


# ------------------------------------------------------------------ folds
def month_of(ms):
    return pd.to_datetime(ms, unit="ms", utc=True).strftime("%Y-%m")


def fold_indices(tau, test_month, purge_ms, start_ms=None):
    m = month_of(tau)
    test = np.flatnonzero(m == test_month)
    if len(test) == 0:
        return None
    t0 = tau[test].min()
    val_end = t0 - purge_ms
    val_start = val_end - VAL_DAYS * 86_400_000
    val = np.flatnonzero((tau >= val_start) & (tau < val_end))
    tr_mask = tau < val_start - purge_ms
    if start_ms is not None:
        tr_mask &= tau >= start_ms
        val = val[tau[val] >= start_ms]
    return np.flatnonzero(tr_mask), val, test


# ----------------------------------------------------------------- targets
def classes(r, c, kind):
    """Economic classes of a realised log return against the cost c (fraction).

    c3: SHORT/NEUTRAL/LONG at +-c.  c5: strong_down < -2c <= down < -c <=
    neutral <= c < up <= 2c < strong_up."""
    y = np.full(len(r), -1)
    ok = np.isfinite(r) & np.isfinite(c)
    if kind == "c3":
        y[ok] = np.where(r[ok] > c[ok], 2, np.where(r[ok] < -c[ok], 0, 1))
    else:
        rr, cc = r[ok], c[ok]
        y[ok] = np.select([rr < -2 * cc, rr < -cc, rr <= cc, rr <= 2 * cc], [0, 1, 2, 3], 4)
    return y


def fit(X, y, Xv, yv, objective, num_class=None, weight=None):
    p = dict(PARAMS, objective=objective)
    if num_class:
        p["num_class"] = num_class
        p["metric"] = "multi_logloss"
    else:
        p["metric"] = "l2" if objective == "regression" else objective
    dtr = lgb.Dataset(X, y, weight=weight, free_raw_data=True)
    dva = lgb.Dataset(Xv, yv, reference=dtr, free_raw_data=True)
    return lgb.train(p, dtr, MAX_ROUNDS, valid_sets=[dva],
                     callbacks=[lgb.early_stopping(EARLY, verbose=False)])


# ------------------------------------------------------------- economics
def select_trades(side, tau, inst, hold_ms):
    """One open position per instrument; overlapping across instruments."""
    order = np.lexsort((tau, inst))
    keep = []
    last = {}
    for i in order:
        if side[i] == 0:
            continue
        if inst[i] in last and tau[i] < last[inst[i]]:
            continue
        keep.append(i)
        last[inst[i]] = tau[i] + hold_ms
    return np.array(sorted(keep, key=lambda i: tau[i]), dtype=np.int64)


def economics(sel, side, r_lat, r0, cost_bps, spread_bps, tau, drift_r=None):
    if len(sel) == 0:
        return dict(trades=0)
    s = side[sel].astype(float)
    gross0 = s * r0[sel] * 1e4
    lat = s * (r_lat[sel] - r0[sel]) * 1e4
    fee = np.full(len(sel), 2 * C.FEE_BPS_PER_SIDE)
    spr = spread_bps[sel]
    imp = np.full(len(sel), 2 * C.IMPACT_BPS_PER_SIDE)
    net = s * r_lat[sel] * 1e4 - cost_bps[sel]
    day = tau[sel] // 86_400_000
    dm = pd.Series(net).groupby(day).mean()
    t_day = dm.mean() / (dm.std(ddof=1) / np.sqrt(len(dm))) if len(dm) > 2 and dm.std() > 0 else np.nan
    out = dict(trades=int(len(sel)), days=int(len(dm)), long_share=float((s > 0).mean()),
               gross=float(gross0.mean()), latency=float(lat.mean()), fee=float(fee.mean()),
               spread=float(spr.mean()), impact=float(imp.mean()), net=float(net.mean()),
               t_day=float(t_day), hit=float((net > 0).mean()))
    if drift_r is not None:
        drift = float(np.nanmean(drift_r) * 1e4)
        bench = out["long_share"] * drift - (1 - out["long_share"]) * drift
        out["drift"] = drift
        out["excess_vs_drift"] = float((s * r_lat[sel] * 1e4).mean() - bench)
    return out


def logloss(p, y, k):
    p = np.clip(p, 1e-12, 1)
    return float(-np.mean(np.log(p[np.arange(len(y)), y])))


def run_variant(name, blocks, horizon, target="c3", latency=1, cost="v2", train_filter=None,
                test_filter=None, start_ms=None, extra=None, months=TEST_MONTHS, log=None,
                keep_preds=False, importance=False):
    """One variant over the folds. Returns per-fold dicts."""
    R = rows()
    tau = R.tau_ms.values
    inst = R.instrument_id.values
    hm = C.HORIZONS[horizon]
    r = label("r_%s_L%d" % (horizon, latency))
    r0 = label("r_%s_L0" % horizon)
    if cost == "v2":
        cbps = cost_col("cost_rt_bps")
    else:
        cbps = np.full(len(R), 16.0)
    spread = cost_col("spread_bps") if cost == "v2" else np.full(len(R), 2.0)
    cf = cbps / 1e4
    purge = (hm + max(C.LATENCIES)) * C.MIN_MS
    res = []
    preds = []
    for mo in months:
        if not keep_preds and (name, mo, horizon) in DONE:
            continue   # already in results.jsonl (resumed run)
        f = fold_indices(tau, mo, purge, start_ms)
        if f is None:
            continue
        tr, va, te = f
        ok = lambda ix: ix[np.isfinite(r[ix]) & np.isfinite(cf[ix])]
        tr, va, te = ok(tr), ok(va), ok(te)
        if train_filter is not None:
            tr = tr[train_filter[tr] > 0]
            va = va[train_filter[va] > 0]
        if len(tr) < 5000 or len(va) < 1000:
            res.append(dict(variant=name, fold=mo, status="INSUFFICIENT", n_train=int(len(tr))))
            continue
        t0 = time.time()
        Xtr, cols = features(blocks, tr, extra)
        Xva, _ = features(blocks, va, extra)
        Xte, _ = features(blocks, te, extra)
        if target in ("c3", "c5"):
            k = 3 if target == "c3" else 5
            ytr, yva, yte = classes(r[tr], cf[tr], target), classes(r[va], cf[va], target), classes(r[te], cf[te], target)
            m = fit(Xtr, ytr, Xva, yva, "multiclass", k)
            # class-conditional mean return, from TRAINING rows only
            mu = np.array([np.nanmean(r[tr][ytr == j]) if (ytr == j).any() else 0.0 for j in range(k)])
            pva, pte = m.predict(Xva), m.predict(Xte)
            er_va, er_te = pva @ mu, pte @ mu
            freq = np.bincount(ytr, minlength=k) / len(ytr)
            ll = logloss(pte, yte, k); ll0 = logloss(np.tile(freq, (len(yte), 1)), yte, k)
            skill = 1 - ll / ll0
        else:
            m = fit(Xtr, r[tr], Xva, r[va], "regression")
            er_va, er_te = m.predict(Xva), m.predict(Xte)
            skill = np.nan
        ic = float(pd.Series(er_te).corr(pd.Series(r[te]), method="spearman"))
        # nested policy margin on validation
        best, best_net = None, -np.inf
        for mg in MARGIN_GRID:
            sd = np.where(er_va * 1e4 - cbps[va] > mg, 1, np.where(-er_va * 1e4 - cbps[va] > mg, -1, 0))
            sel = select_trades(sd, tau[va], inst[va], hm * C.MIN_MS)
            if len(sel) >= 30:
                e = economics(sel, sd, r[va], r0[va], cbps[va], spread[va], tau[va])
                if e["net"] > best_net:
                    best, best_net = mg, e["net"]
        out = dict(variant=name, fold=mo, horizon=horizon, target=target, latency=latency,
                   n_train=int(len(tr)), n_test=int(len(te)), iters=int(m.best_iteration),
                   ll_skill=float(skill), ic=ic, margin=best, val_net=float(best_net) if best is not None else np.nan)
        for label_, mg in (("m0", 0.0), ("mv", best)):
            if mg is None:
                out[label_] = dict(trades=0)
                continue
            sd = np.where(er_te * 1e4 - cbps[te] > mg, 1, np.where(-er_te * 1e4 - cbps[te] > mg, -1, 0))
            if test_filter is not None:
                sd = np.where(test_filter[te] > 0, sd, 0)
            sel = select_trades(sd, tau[te], inst[te], hm * C.MIN_MS)
            out[label_] = economics(sel, sd, r[te], r0[te], cbps[te], spread[te], tau[te], drift_r=r[te])
        # Ranking diagnostic, independent of calibration: the extreme 1% of the
        # predicted return, thresholds from the VALIDATION distribution.
        qlo, qhi = np.quantile(er_va, 0.01), np.quantile(er_va, 0.99)
        sdq = np.where(er_te >= qhi, 1, np.where(er_te <= qlo, -1, 0))
        if test_filter is not None:
            sdq = np.where(test_filter[te] > 0, sdq, 0)
        selq = select_trades(sdq, tau[te], inst[te], hm * C.MIN_MS)
        out["q1pct"] = economics(selq, sdq, r[te], r0[te], cbps[te], spread[te], tau[te], drift_r=r[te])
        # the same decisions under every entry latency (no refit): what the
        # delay between the bar close and the fill costs, per horizon
        mg = best if best is not None else 0.0
        sd = np.where(er_te * 1e4 - cbps[te] > mg, 1, np.where(-er_te * 1e4 - cbps[te] > mg, -1, 0))
        if test_filter is not None:
            sd = np.where(test_filter[te] > 0, sd, 0)
        lat = {}
        for L in C.LATENCIES:
            rl = label("r_%s_L%d" % (horizon, L))[te]
            sel = select_trades(sd, tau[te], inst[te], hm * C.MIN_MS)
            okl = sel[np.isfinite(rl[sel])]
            e = economics(okl, sd, rl, r0[te], cbps[te], spread[te], tau[te])
            lat["L%d" % L] = {k: e.get(k) for k in ("trades", "gross", "latency", "net", "t_day")}
        out["by_latency"] = lat
        if importance:
            gain = m.feature_importance("gain")
            out["gain_top"] = [(cols[i], float(gain[i])) for i in np.argsort(-gain)[:40]]
            out["_gain"] = dict(zip(cols, map(float, gain)))
        if keep_preds:
            preds.append(pd.DataFrame({"row": te, "er": er_te, "side": sd, "sideq": sdq, "fold": mo,
                                       "qlo": qlo, "qhi": qhi, "margin": mg}))
            out["_model"] = m
            out["_cols"] = cols
        out["secs"] = round(time.time() - t0, 1)
        res.append(out)
        if log:
            log.write(json.dumps({k: v for k, v in out.items() if not k.startswith("_")}) + "\n"); log.flush()
        del Xtr, Xva, Xte
    if keep_preds:
        return res, (pd.concat(preds, ignore_index=True) if preds else None)
    return res


def evaluate_subset(preds, horizon, mask, latency=1, cost="v2"):
    """Re-evaluate stored OOS decisions on a subset of test rows (no refit)."""
    R = rows()
    tau = R.tau_ms.values
    inst = R.instrument_id.values
    hm = C.HORIZONS[horizon]
    r = label("r_%s_L%d" % (horizon, latency))
    r0 = label("r_%s_L0" % horizon)
    cbps = cost_col("cost_rt_bps") if cost == "v2" else np.full(len(R), 16.0)
    spread = cost_col("spread_bps") if cost == "v2" else np.full(len(R), 2.0)
    out = []
    for mo, g in preds.groupby("fold"):
        rowsx = g.row.values
        keep = mask[rowsx] > 0
        res = {"fold": mo, "n_test": int(keep.sum())}
        for key, col in (("mv", "side"), ("q1pct", "sideq")):
            sd = np.zeros(len(R), dtype=int)
            sd[rowsx[keep]] = g[col].values[keep]
            idx = rowsx[keep]
            sel = select_trades(sd[idx], tau[idx], inst[idx], hm * C.MIN_MS)
            res[key] = economics(sel, sd[idx], r[idx], r0[idx], cbps[idx], spread[idx], tau[idx], drift_r=r[idx])
        out.append(res)
    return out
