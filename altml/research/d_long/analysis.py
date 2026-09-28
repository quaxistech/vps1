"""D_long_v1 analyses that need more than one fold record each:

  stability   gain / permutation / SHAP (LightGBM pred_contrib) per fold,
              instrument group and trend regime -> which features are useful
              REPEATEDLY; then OOS ablation of the stable subset vs the full state
  redundancy  |Spearman| >= 0.9 clusters on a training sample; cluster-level
              OOS ablation for the largest clusters
  slices      the stored OOS decisions by instrument, horizon, regime, volatility
              and liquidity tercile, direction, setup, hour, weekday (diagnostic
              only: nothing here selects a model)
  pooled      one multiclass model over the three horizons (horizon a feature)
              against the three specialised models, same folds and rows
  micro/drv   blocks that exist for weeks, not months: one chronological split,
              reported as INDICATIVE / INSUFFICIENT, never used to select

Importance is a diagnostic, never evidence of cause.
Usage: DLONG=<blocks> python analysis.py <task> [...]
"""

import json
import os
import pickle
import sys

import lightgbm as lgb
import numpy as np
import pandas as pd

import common as C
import exp as E
from run_all import STATE

OUT = os.path.join(E.DS, "analysis")
os.makedirs(OUT, exist_ok=True)


def _log(name, obj):
    with open(os.path.join(OUT, name + ".json"), "w") as f:
        json.dump(obj, f, indent=1, default=float)


def stability(horizons=("15m", "1h", "4h"), top_k=40, perm_rows=20000, shap_rows=5000):
    R = E.rows()
    tau = R.tau_ms.values
    inst = R.instrument_id.values
    su = E.setup_descriptors()
    res = {}
    for h in horizons:
        hm = C.HORIZONS[h]
        r = E.label("r_%s_L1" % h)
        cf = E.cost_col("cost_rt_bps") / 1e4
        purge = (hm + max(C.LATENCIES)) * C.MIN_MS
        st1h = np.asarray(E.block("tl_1h")[0][:, E.block("tl_1h")[1].index("1h:ema_stack")], dtype=float)
        per_fold = []
        for mo in E.TEST_MONTHS:
            tr, va, te = E.fold_indices(tau, mo, purge)
            ok = lambda ix: ix[np.isfinite(r[ix])]
            tr, va, te = ok(tr), ok(va), ok(te)
            Xtr, cols = E.features(STATE, tr)
            Xva, _ = E.features(STATE, va)
            Xte, _ = E.features(STATE, te)
            ytr, yva, yte = (E.classes(r[i], cf[i], "c3") for i in (tr, va, te))
            m = E.fit(Xtr, ytr, Xva, yva, "multiclass", 3)
            gain = m.feature_importance("gain")
            gain = gain / gain.sum()
            # permutation on a test sample: increase in multi-logloss
            rng = np.random.default_rng(42)
            sidx = rng.choice(len(te), size=min(perm_rows, len(te)), replace=False)
            Xs, ys = Xte[sidx], yte[sidx]
            base = E.logloss(m.predict(Xs), ys, 3)
            gtop = np.argsort(-gain)[:top_k * 2]
            perm = np.zeros(len(cols))
            for j in gtop:
                Xp = Xs.copy()
                Xp[:, j] = Xp[rng.permutation(len(Xp)), j]
                perm[j] = E.logloss(m.predict(Xp), ys, 3) - base
            # SHAP: mean |contribution| summed over the three classes
            shidx = sidx[:shap_rows]
            contrib = m.predict(Xte[shidx], pred_contrib=True)
            k = len(cols) + 1
            shap = np.abs(contrib.reshape(len(shidx), 3, k)[:, :, :-1]).sum(axis=1)
            shap_mean = shap.mean(axis=0)
            # SHAP by trend regime and by instrument group
            reg = st1h[te][shidx]
            big = np.isin(inst[te][shidx], (1, 2))           # BTC, ETH
            by = {"trend_up": shap[reg == 1].mean(axis=0) if (reg == 1).any() else None,
                  "trend_down": shap[reg == -1].mean(axis=0) if (reg == -1).any() else None,
                  "mixed": shap[reg == 0].mean(axis=0) if (reg == 0).any() else None,
                  "btc_eth": shap[big].mean(axis=0), "alts": shap[~big].mean(axis=0)}
            per_fold.append({"fold": mo, "gain": gain, "perm": perm, "shap": shap_mean,
                             "by": {kk: v for kk, v in by.items() if v is not None}})
            print("stability", h, mo, flush=True)
        # repeated usefulness: share of folds a feature is in the top_k by each measure
        n = len(cols)
        rank_share = {}
        for meas in ("gain", "perm", "shap"):
            hits = np.zeros(n)
            for pf in per_fold:
                hits[np.argsort(-pf[meas])[:top_k]] += 1
            rank_share[meas] = hits / len(per_fold)
        seg_hits = np.zeros(n)
        segs = 0
        for pf in per_fold:
            for kk, v in pf["by"].items():
                seg_hits[np.argsort(-v)[:top_k]] += 1
                segs += 1
        stable = [cols[i] for i in range(n)
                  if sum(rank_share[mm][i] >= 0.6 for mm in ("gain", "perm", "shap")) >= 2
                  and seg_hits[i] / max(segs, 1) >= 0.5]
        res[h] = {"n_features": n, "stable": stable,
                  "top_by_mean_shap": [cols[i] for i in np.argsort(-np.mean([pf["shap"] for pf in per_fold], axis=0))[:30]],
                  "rank_share": {mm: {cols[i]: float(v[i]) for i in np.argsort(-v)[:60]} for mm, v in rank_share.items()},
                  "perm_mean_top": {cols[i]: float(np.mean([pf["perm"][i] for pf in per_fold]))
                                    for i in np.argsort(-np.mean([pf["perm"] for pf in per_fold], axis=0))[:30]}}
        with open(os.path.join(OUT, "stability_raw_%s.pkl" % h), "wb") as f:
            pickle.dump({"cols": cols, "per_fold": per_fold}, f)
    _log("stability", res)
    return res


def stable_ablation(log):
    st = json.load(open(os.path.join(OUT, "stability.json")))
    for h, v in st.items():
        keep = set(v["stable"])
        if not keep:
            log.write(json.dumps({"variant": "stable_subset", "horizon": h, "status": "EMPTY_STABLE_SET"}) + "\n")
            continue
        # build a boolean column mask over STATE by name
        extra = {}
        for b in STATE:
            arr, cols = E.block(b)
            for i, c in enumerate(cols):
                if c in keep:
                    extra[c] = np.asarray(arr[:, i], dtype=np.float64)
        E.run_variant("stable_subset", [], h, "c3", extra=extra, log=log)


def redundancy(sample=60000, thr=0.9):
    R = E.rows()
    rng = np.random.default_rng(7)
    idx = np.sort(rng.choice(len(R), size=sample, replace=False))
    X, cols = E.features(STATE, idx)
    df = pd.DataFrame(X, columns=cols)
    corr = df.rank().corr().abs().values
    np.fill_diagonal(corr, 0)
    # greedy single-link clusters at |rho| >= thr
    parent = list(range(len(cols)))
    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for i, j in zip(*np.where(np.triu(corr >= thr))):
        parent[find(i)] = find(j)
    groups = {}
    for i in range(len(cols)):
        groups.setdefault(find(i), []).append(cols[i])
    clusters = sorted([g for g in groups.values() if len(g) > 1], key=len, reverse=True)
    _log("redundancy", {"threshold": thr, "n_features": len(cols), "n_clusters": len(clusters),
                        "features_in_clusters": int(sum(len(c) for c in clusters)),
                        "clusters": clusters[:40]})
    return clusters


def cluster_ablation(log, clusters_to_test=3):
    red = json.load(open(os.path.join(OUT, "redundancy.json")))
    for ci, cl in enumerate(red["clusters"][:clusters_to_test]):
        drop = set(cl[1:])                  # keep one representative
        extra = {}
        for b in STATE:
            arr, cols = E.block(b)
            for i, c in enumerate(cols):
                if c not in drop:
                    extra[c] = np.asarray(arr[:, i], dtype=np.float64)
        for h in ("1h", "4h"):
            E.run_variant("dedup_cluster_%d" % ci, [], h, "c3", extra=extra, log=log,
                          months=("2026-07", "2026-08", "2026-09"))


def slices():
    R = E.rows()
    tau = R.tau_ms.values
    inst = R.instrument_id.values
    su = E.setup_descriptors()
    natr = np.asarray(E.block("tl_1h")[0][:, E.block("tl_1h")[1].index("1h:NATR_14_outReal")], dtype=float)
    st1h = np.asarray(E.block("tl_1h")[0][:, E.block("tl_1h")[1].index("1h:ema_stack")], dtype=float)
    qv = np.asarray(E.block("cdl")[0][:, E.block("cdl")[1].index("qv_anom_1h")], dtype=float)
    out = {}
    for h in ("15m", "1h", "4h"):
        p = os.path.join(E.DS, "preds_state_%s.pkl" % h)
        if not os.path.exists(p):
            continue
        preds = pickle.load(open(p, "rb"))["preds"]
        r = E.label("r_%s_L1" % h)
        r0 = E.label("r_%s_L0" % h)
        cb = E.cost_col("cost_rt_bps")
        sp = E.cost_col("spread_bps")
        hm = C.HORIZONS[h]
        rows_ = preds.row.values
        dfq = pd.DataFrame({"row": rows_, "side": preds.sideq.values, "fold": preds.fold.values})
        # decisions actually taken (one position per instrument)
        sd = np.zeros(len(R), dtype=int)
        sd[rows_] = dfq.side.values
        sel = E.select_trades(sd[rows_], tau[rows_], inst[rows_], hm * C.MIN_MS)
        tr = rows_[sel]
        tdf = pd.DataFrame({
            "net": sd[tr] * r[tr] * 1e4 - cb[tr], "gross": sd[tr] * r0[tr] * 1e4,
            "instrument": inst[tr], "side": np.where(sd[tr] > 0, "long", "short"),
            "trend_1h": st1h[tr],
            "vol_tercile": pd.qcut(pd.Series(natr[tr]).rank(method="first"), 3, labels=["low", "mid", "high"]).astype(str),
            "liq_tercile": pd.qcut(pd.Series(qv[tr]).rank(method="first"), 3, labels=["low", "mid", "high"]).astype(str),
            "setup_any": su["su_any"][tr],
            "hour": (tau[tr] // 3_600_000) % 24, "weekday": ((tau[tr] // 86_400_000) + 4) % 7})
        agg = {}
        for key in ("instrument", "side", "trend_1h", "vol_tercile", "liq_tercile", "setup_any", "hour", "weekday"):
            g = tdf.groupby(key).agg(trades=("net", "size"), net=("net", "mean"), gross=("gross", "mean"))
            agg[key] = g.round(2).reset_index().to_dict(orient="records")
        # Real AltStrategy candidates (recorded from 2026-09-11): the OOS model's
        # view of states a detector described vs states none did, same fold.
        ca, cc = E.block("cand")
        cand = np.asarray(ca[:, cc.index("cand_any")], dtype=float)
        er = preds.er.values
        sep = np.isfinite(cand[rows_]) & np.isfinite(r[rows_])
        realc = {}
        for lab_, msk in (("detector_fired", sep & (cand[rows_] > 0)), ("no_detector", sep & (cand[rows_] == 0))):
            if msk.sum() > 100:
                realc[lab_] = {"rows": int(msk.sum()),
                               "ic": float(pd.Series(er[msk]).corr(pd.Series(r[rows_][msk]), method="spearman")),
                               "mean_abs_return_bps": float(np.nanmean(np.abs(r[rows_][msk])) * 1e4)}
        out[h] = {"decision": "extreme 1% of predicted return (validation thresholds)",
                  "trades": int(len(tdf)), "net": float(tdf.net.mean()), "slices": agg,
                  "real_candidates_sep11_24": realc}
    _log("slices", out)
    return out


def pooled(log, months=E.TEST_MONTHS):
    """One model over the three horizons vs three specialised: rows stacked with a
    horizon feature; each horizon scored on its own test rows."""
    R = E.rows()
    tau = R.tau_ms.values
    inst = R.instrument_id.values
    cf = E.cost_col("cost_rt_bps") / 1e4
    cb = E.cost_col("cost_rt_bps")
    sp = E.cost_col("spread_bps")
    purge = (240 + max(C.LATENCIES)) * C.MIN_MS
    for mo in months:
        f = E.fold_indices(tau, mo, purge)
        if f is None:
            continue
        tr, va, te = f
        Xs, ys, Xv, yv, tests = [], [], [], [], {}
        for hi, h in enumerate(("15m", "1h", "4h")):
            r = E.label("r_%s_L1" % h)
            trh, vah, teh = (ix[np.isfinite(r[ix])] for ix in (tr, va, te))
            Xa, cols = E.features(STATE, trh)
            Xs.append(np.hstack([Xa, np.full((len(trh), 1), hi, np.float32)]))
            ys.append(E.classes(r[trh], cf[trh], "c3"))
            Xb, _ = E.features(STATE, vah)
            Xv.append(np.hstack([Xb, np.full((len(vah), 1), hi, np.float32)]))
            yv.append(E.classes(r[vah], cf[vah], "c3"))
            tests[h] = (teh, hi, trh, ys[-1])
        m = E.fit(np.vstack(Xs), np.concatenate(ys), np.vstack(Xv), np.concatenate(yv), "multiclass", 3)
        del Xs, Xv
        for h, (teh, hi, trh, ytrh) in tests.items():
            r = E.label("r_%s_L1" % h)
            r0 = E.label("r_%s_L0" % h)
            Xt, _ = E.features(STATE, teh)
            p = m.predict(np.hstack([Xt, np.full((len(teh), 1), hi, np.float32)]))
            mu = np.array([np.nanmean(r[trh][ytrh == j]) for j in range(3)])
            er = p @ mu
            yte = E.classes(r[teh], cf[teh], "c3")
            freq = np.bincount(ytrh, minlength=3) / len(ytrh)
            skill = 1 - E.logloss(p, yte, 3) / E.logloss(np.tile(freq, (len(yte), 1)), yte, 3)
            ic = float(pd.Series(er).corr(pd.Series(r[teh]), method="spearman"))
            hm = C.HORIZONS[h]
            qlo, qhi = np.quantile(er, 0.01), np.quantile(er, 0.99)   # test-quantile diagnostic only
            sd = np.where(er >= qhi, 1, np.where(er <= qlo, -1, 0))
            sel = E.select_trades(sd, tau[teh], inst[teh], hm * C.MIN_MS)
            eco = E.economics(sel, sd, r[teh], r0[teh], cb[teh], sp[teh], tau[teh], drift_r=r[teh])
            log.write(json.dumps({"variant": "pooled_3h", "fold": mo, "horizon": h, "ll_skill": float(skill),
                                  "ic": ic, "q1pct_test_quantiles": eco}) + "\n")
            log.flush()
        print("pooled", mo, flush=True)


def pooled_light(log, months=("2026-07", "2026-08", "2026-09"), share=3):
    """One model over the three horizons vs three specialised -- on the SAME rows.

    The stacked version (pooled) held three horizons' full training sets at
    once and was OOM-killed at 3.7 GB on the 7.2 GB workstation (2026-09-26).
    Here each horizon's training rows are a deterministic 1/`share` subsample
    (every share-th row, offset by horizon), the pooled model is fitted on the
    three subsamples stacked (the size of ONE full horizon), and each
    specialised model on its own subsample alone -- so the comparison is
    between architectures on identical rows. Validation and test are full.
    Scored per horizon: log-loss skill against the training class
    frequencies, AUC of P(long) - P(short) on the sign of the return, rank IC
    of E[r]."""
    from sklearn.metrics import roc_auc_score
    R = E.rows()
    tau = R.tau_ms.values
    cf = E.cost_col("cost_rt_bps") / 1e4
    purge = (240 + max(C.LATENCIES)) * C.MIN_MS
    H3 = ("15m", "1h", "4h")
    for mo in months:
        f = E.fold_indices(tau, mo, purge)
        if f is None:
            continue
        tr, va, te = f
        sub, fit_single, parts = {}, {}, {"X": [], "y": [], "Xv": [], "yv": []}
        for hi, h in enumerate(H3):
            r = E.label("r_%s_L1" % h)
            trh = tr[np.isfinite(r[tr])][hi::share]
            vah = va[np.isfinite(r[va])]
            ytr = E.classes(r[trh], cf[trh], "c3")
            yva = E.classes(r[vah], cf[vah], "c3")
            Xa, _ = E.features(STATE, trh)
            Xb, _ = E.features(STATE, vah)
            fit_single[h] = E.fit(Xa, ytr, Xb, yva, "multiclass", 3)
            parts["X"].append(np.hstack([Xa, np.full((len(trh), 1), hi, np.float32)]))
            parts["y"].append(ytr)
            parts["Xv"].append(np.hstack([Xb, np.full((len(vah), 1), hi, np.float32)]))
            parts["yv"].append(yva)
            sub[h] = (trh, ytr)
            del Xa, Xb
        pooled_m = E.fit(np.vstack(parts["X"]), np.concatenate(parts["y"]), np.vstack(parts["Xv"]),
                         np.concatenate(parts["yv"]), "multiclass", 3)
        del parts
        for hi, h in enumerate(H3):
            r = E.label("r_%s_L1" % h)
            teh = te[np.isfinite(r[te])]
            trh, ytr = sub[h]
            Xt, _ = E.features(STATE, teh)
            yte = E.classes(r[teh], cf[teh], "c3")
            freq = np.bincount(ytr, minlength=3) / len(ytr)
            ll0 = E.logloss(np.tile(freq, (len(yte), 1)), yte, 3)
            mu = np.array([np.nanmean(r[trh][ytr == j]) for j in range(3)])
            ok = r[teh] != 0
            res = {"variant": "pooled_vs_three", "fold": mo, "horizon": h, "share": share,
                   "n_train_per_horizon": int(len(trh))}
            for arch, p in (("three", fit_single[h].predict(Xt)),
                            ("pooled", pooled_m.predict(np.hstack([Xt, np.full((len(teh), 1), hi, np.float32)])))):
                s = p[:, 2] - p[:, 0]
                res[arch] = {"ll_skill": float(1 - E.logloss(p, yte, 3) / ll0),
                             "auc": float(roc_auc_score((r[teh][ok] > 0).astype(int), s[ok])),
                             "ic": float(pd.Series(p @ mu).corr(pd.Series(r[teh]), method="spearman"))}
            log.write(json.dumps(res) + "\n")
            log.flush()
            del Xt
        print("pooled_light", mo, flush=True)


def short_window(log):
    """Micro (book, from 2026-09-01) and derivatives (Bitget funding/OI/spread,
    effectively from 2026-08-22): one chronological split each, INDICATIVE."""
    R = E.rows()
    tau = R.tau_ms.values
    for name, blk, start, split in (("micro", "mic", 1_788_220_800_000, 1_789_603_200_000),        # Sep 1 / Sep 17
                                    ("derivatives", "drv", 1_787_356_800_000, 1_789_171_200_000)):  # Aug 22 / Sep 12
        for h in ("15m", "1h"):
            for variant, blocks in ((name + "0_state", STATE), (name + "1_plus", STATE + [blk])):
                r = E.label("r_%s_L1" % h)
                cf = E.cost_col("cost_rt_bps") / 1e4
                hm = C.HORIZONS[h]
                purge = (hm + max(C.LATENCIES)) * C.MIN_MS
                tr_all = np.flatnonzero((tau >= start) & (tau < split - purge) & np.isfinite(r))
                te = np.flatnonzero((tau >= split) & np.isfinite(r))
                va = tr_all[-len(tr_all) // 5:]
                tr = tr_all[:-len(tr_all) // 5]
                Xtr, _ = E.features(blocks, tr)
                Xva, _ = E.features(blocks, va)
                Xte, _ = E.features(blocks, te)
                ytr, yva, yte = (E.classes(r[i], cf[i], "c3") for i in (tr, va, te))
                m = E.fit(Xtr, ytr, Xva, yva, "multiclass", 3)
                p = m.predict(Xte)
                freq = np.bincount(ytr, minlength=3) / len(ytr)
                skill = 1 - E.logloss(p, yte, 3) / E.logloss(np.tile(freq, (len(yte), 1)), yte, 3)
                mu = np.array([np.nanmean(r[tr][ytr == j]) for j in range(3)])
                ic = float(pd.Series(p @ mu).corr(pd.Series(r[te]), method="spearman"))
                log.write(json.dumps({"variant": variant, "horizon": h, "status": "INDICATIVE",
                                      "n_train": int(len(tr)), "n_test": int(len(te)),
                                      "test_days": float((tau[te].max() - tau[te].min()) / 86_400_000),
                                      "ll_skill": float(skill), "ic": ic}) + "\n")
                log.flush()


if __name__ == "__main__":
    with open(os.path.join(E.DS, "results.jsonl"), "a") as log:
        for task in sys.argv[1:]:
            if task == "stability":
                stability()
            elif task == "stable_ablation":
                stable_ablation(log)
            elif task == "redundancy":
                redundancy()
            elif task == "cluster_ablation":
                cluster_ablation(log)
            elif task == "slices":
                slices()
            elif task == "pooled":
                pooled(log)
            elif task == "pooled_light":
                pooled_light(log)
            elif task == "short_window":
                short_window(log)
