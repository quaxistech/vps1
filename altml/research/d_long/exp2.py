"""Market-outcome targets for the VPS1 LightGBM (final stage).

VPS1 predicts what the MARKET does after a state; whether that pays after
today's costs is VPS2's question. This compares, on the same walk-forward folds,
rows and features as exp.py (STATE blocks, entry latency L1), the targets a
market-intelligence model can be asked for, scored on MARKET metrics only:

  direction   3-class. Two class boundaries:
                cost  -- |r| against the all-in round trip (the production
                         econ label; an economic definition)
                vol   -- terciles of r / sigma, sigma the trailing 1h realised
                         volatility scaled to the horizon; the tercile cut is
                         fitted on the TRAINING rows only (a market definition:
                         "a move large for the current volatility")
              scored by AUC of P(up)-P(down) on sign(r), rank IC of E[r],
              log-loss skill against the training class frequencies, Brier
              skill, and ECE per class (10 equal-count bins) under three
              calibrators fitted on the validation slice: temperature,
              one-vs-rest isotonic (renormalised), none.
  magnitude   LightGBM L2 regressions on log(1 + bps) of the favourable-up
              excursion U = MFE_long, the down excursion D = -MAE_long and |r|.
              Baselines: (a) a log-linear fit on the trailing realised
              volatility (the naive volatility forecast), (b) the classifier's
              class-conditional means (what production publishes today).
              Scored by Spearman and out-of-sample R^2 on the log scale.
  timing      time to the up / down extreme (minutes): L2 regression on log;
              Spearman against realised, baseline the training median.

Nothing here is selected on a test fold: calibrators and tercile cuts are fitted
on training / validation rows; every number reported is test-fold.
"""
import json
import os
import sys
import time

import lightgbm as lgb
import numpy as np
import pandas as pd
from scipy.stats import spearmanr
from sklearn.isotonic import IsotonicRegression
from sklearn.metrics import roc_auc_score

import common as C
import exp as E

STATE = ["tl_1m", "tl_5m", "tl_15m", "tl_30m", "tl_1h", "tl_4h", "age", "xtf", "cdl"]
OUT = os.path.join(E.DS, "analysis")


def softmax(z, t=1.0):
    z = z / t
    z = z - z.max(axis=1, keepdims=True)
    e = np.exp(z)
    return e / e.sum(axis=1, keepdims=True)


def fit_temperature(raw, y):
    best, bt = np.inf, 1.0
    for t in np.exp(np.linspace(np.log(0.3), np.log(3.0), 61)):
        p = softmax(raw, t)
        ll = -np.mean(np.log(np.clip(p[np.arange(len(y)), y], 1e-12, 1)))
        if ll < best:
            best, bt = ll, t
    return bt


def ece(p, y, k, bins=10):
    """Expected calibration error of class k, equal-count bins."""
    pk = p[:, k]
    hit = (y == k).astype(float)
    order = np.argsort(pk)
    e = 0.0
    for chunk in np.array_split(order, bins):
        if len(chunk) == 0:
            continue
        e += len(chunk) / len(pk) * abs(pk[chunk].mean() - hit[chunk].mean())
    return float(e)


def dir_metrics(p, y, r, prior):
    ok = np.isfinite(r)
    s = p[:, 2] - p[:, 0]
    sg = r[ok] != 0
    auc = roc_auc_score((r[ok][sg] > 0).astype(int), s[ok][sg]) if sg.sum() > 100 else np.nan
    ll = -np.mean(np.log(np.clip(p[np.arange(len(y)), y], 1e-12, 1)))
    ll0 = -np.mean(np.log(np.clip(prior[y], 1e-12, 1)))
    oh = np.eye(3)[y]
    br = np.mean(np.sum((p - oh) ** 2, axis=1))
    br0 = np.mean(np.sum((prior[None, :] - oh) ** 2, axis=1))
    return dict(auc=float(auc), ll_skill=float(1 - ll / ll0), brier_skill=float(1 - br / br0),
                ece_down=ece(p, y, 0), ece_flat=ece(p, y, 1), ece_up=ece(p, y, 2))


def iso_calibrate(pv, yv, pt):
    out = np.zeros_like(pt)
    for k in range(3):
        ir = IsotonicRegression(out_of_bounds="clip", y_min=1e-4, y_max=1 - 1e-4)
        ir.fit(pv[:, k], (yv == k).astype(float))
        out[:, k] = ir.predict(pt[:, k])
    return out / out.sum(axis=1, keepdims=True)


def main(horizons=("15m", "1h", "4h"), months=E.TEST_MONTHS):
    R = E.rows()
    tau = R.tau_ms.values
    cf = E.cost_col("cost_rt_bps") / 1e4
    rv1h = np.asarray(E.block("cdl")[0][:, E.block("cdl")[1].index("rv_1h")], dtype=np.float64)
    log = open(os.path.join(OUT, "targets.jsonl"), "a")
    for h in horizons:
        hm = C.HORIZONS[h]
        r = E.label("r_%s_L1" % h)
        mfe, mae = E.label("mfe_%s" % h), E.label("mae_%s" % h)
        tup, tdn = E.label("tmfe_%s" % h), E.label("tmae_%s" % h)
        U = np.log1p(np.expm1(mfe) * 1e4)                 # log(1 + up excursion, bps)
        D = np.log1p(-np.expm1(mae) * 1e4)                # log(1 + down excursion, bps)
        A = np.log1p(np.abs(np.expm1(r)) * 1e4)           # log(1 + |return|, bps)
        sigma = rv1h * np.sqrt(hm)                        # trailing 1m-return std scaled to h
        z = r / sigma
        purge = (hm + max(C.LATENCIES)) * C.MIN_MS
        for mo in months:
            t0 = time.time()
            f = E.fold_indices(tau, mo, purge)
            if f is None:
                continue
            ok = np.isfinite(r) & np.isfinite(U) & np.isfinite(D) & np.isfinite(sigma) & (sigma > 0)
            tr, va, te = (ix[ok[ix]] for ix in f)
            Xtr, _ = E.features(STATE, tr)
            Xva, _ = E.features(STATE, va)
            Xte, _ = E.features(STATE, te)
            res = dict(variant="targets", fold=mo, horizon=h, n_train=int(len(tr)), n_test=int(len(te)))

            # ---- direction, two boundaries
            q1, q2 = np.quantile(z[tr], [1 / 3, 2 / 3])
            labels = {
                "cost": E.classes(r, cf, "c3"),
                "vol": np.where(z > q2, 2, np.where(z < q1, 0, 1)),
            }
            res["vol_tercile_cuts"] = [float(q1), float(q2)]
            for name, y in labels.items():
                m = E.fit(Xtr, y[tr], Xva, y[va], "multiclass", 3)
                rv_, rt_ = m.predict(Xva, raw_score=True), m.predict(Xte, raw_score=True)
                prior = np.bincount(y[tr], minlength=3) / len(tr)
                T = fit_temperature(rv_, y[va])
                p_raw, p_t = softmax(rt_), softmax(rt_, T)
                p_iso = iso_calibrate(softmax(rv_, T), y[va], p_t)
                # class-conditional means (training rows) -> E[r], E[U], E[D]
                cm = {k: np.array([np.nanmean(v[tr][y[tr] == c]) for c in range(3)])
                      for k, v in (("r", np.expm1(r) * 1e4), ("U", np.expm1(U)), ("D", np.expm1(D)))}
                er = p_t @ cm["r"]
                ic = spearmanr(er, r[te]).correlation
                res[name] = dict(iters=int(m.best_iteration), temperature=float(T), ic=float(ic),
                                 raw=dir_metrics(p_raw, y[te], r[te], prior),
                                 temp=dir_metrics(p_t, y[te], r[te], prior),
                                 iso=dir_metrics(p_iso, y[te], r[te], prior),
                                 classmean_U_spearman=float(spearmanr(p_t @ cm["U"], U[te]).correlation),
                                 classmean_D_spearman=float(spearmanr(p_t @ cm["D"], D[te]).correlation))
                if name == "vol":
                    keep = dict(row=te, p_down=p_t[:, 0], p_flat=p_t[:, 1], p_up=p_t[:, 2],
                                p_iso_up=p_iso[:, 2], p_iso_down=p_iso[:, 0], er=er)

            # ---- magnitude and timing regressions
            lv = np.log(sigma)
            # EXP2_LEAN=1: only the two magnitude targets production can serve
            # (timing was near-unpredictable at 15m, rho ~0.1; |r| duplicates U/D).
            regs = (("U", U), ("D", D)) if os.environ.get("EXP2_LEAN") == "1" else \
                (("U", U), ("D", D), ("A", A), ("t_up", np.log(tup)), ("t_down", np.log(tdn)))
            for name, tgt in regs:
                okt = np.isfinite(tgt)
                a, b, c = tr[okt[tr]], va[okt[va]], te[okt[te]]
                m = E.fit(Xtr[okt[tr]], tgt[a], Xva[okt[va]], tgt[b], "regression")
                pr = m.predict(Xte[okt[te]])
                ss = np.sum((tgt[c] - tgt[c].mean()) ** 2)
                if name.startswith("t_"):
                    base = np.full(len(c), np.median(tgt[a]))
                else:
                    A_ = np.column_stack([np.ones(len(a)), lv[a]])
                    coef = np.linalg.lstsq(A_, tgt[a], rcond=None)[0]
                    base = coef[0] + coef[1] * lv[c]
                res[name] = dict(iters=int(m.best_iteration),
                                 spearman=float(spearmanr(pr, tgt[c]).correlation),
                                 r2=float(1 - np.sum((tgt[c] - pr) ** 2) / ss),
                                 base_spearman=float(spearmanr(base, tgt[c]).correlation) if not name.startswith("t_") else float("nan"),
                                 base_r2=float(1 - np.sum((tgt[c] - base) ** 2) / ss))
                keep["pred_" + name] = np.full(len(te), np.nan)
                keep["pred_" + name][okt[te]] = pr
            res["secs"] = round(time.time() - t0, 1)
            log.write(json.dumps(res) + "\n")
            log.flush()
            pd.DataFrame(keep).assign(fold=mo).to_pickle(os.path.join(OUT, "targets_%s_%s.pkl" % (h, mo)))
            print(h, mo, "done", res["secs"], flush=True)
    return 0


if __name__ == "__main__":
    hs = tuple(sys.argv[1].split(",")) if len(sys.argv) > 1 else ("15m", "1h", "4h")
    sys.exit(main(hs))
