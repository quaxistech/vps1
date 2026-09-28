"""Lead time, event coverage and signal precision of VPS1's live setup signals.

Inputs (exported from VPS1, read-only): every AltStrategy candidate since
2026-09-11, its decision context, and Bitget 1m candles of the 27 decision
instruments. Everything below is MARKET behaviour, not execution.

Definitions (per instrument, horizon h in 15 / 60 / 240 minutes):
  sigma(t)       std of 1m log returns over the trailing 24 h, point-in-time
  significant    |log(c[t+h] / c[t])| >= 2 * sigma(t) * sqrt(h): a two-sigma
                 move over the horizon starting at close t (market definition,
                 cost-free). Events are taken greedily, non-overlapping.
  covered        a candidate on the instrument, same direction, published in
                 [start - h, start] (early: BEFORE the move began) or in
                 [start - h, start + h/2] (any: before or during its first half)
  hit            a candidate followed, within its horizon, by a favourable
                 excursion >= 2 sigma sqrt(h); adverse: an adverse one >= that
  lead           start - publication (minutes; > 0 = the signal came first)
  already_moved  the move in the signal's direction over the h minutes BEFORE
                 publication, in sigma*sqrt(h) units (how late the signal was)
"""
import json
import sys

import numpy as np
import pandas as pd

D = "/home/quaxis/altdata/leadtime"
H = (15, 60, 240)
MIN = 60_000


def horizon_of(expected_ms):
    m = expected_ms / MIN
    return 15 if m <= 30 else (60 if m <= 120 else 240)


def main():
    cand = pd.read_csv(f"{D}/lt_candidates.csv.gz")
    ctx = pd.read_csv(f"{D}/lt_contexts.csv.gz")
    cdl = pd.read_csv(f"{D}/lt_candles.csv.gz")
    cand = cand.merge(ctx[["context_id", "event_time_ms", "available_at_ms"]], on="context_id", how="left")
    cand["side"] = cand.side.str.upper()
    out = {"period": [int(cand.created_at_ms.min()), int(cand.created_at_ms.max())],
           "candidates": int(len(cand)), "instruments": int(cdl.instrument_id.nunique())}
    # latency from the CLOSE of the 15m decision bar (event_time is its open)
    # to the candidate, and to the context becoming available
    lat = (cand.created_at_ms - (cand.event_time_ms + 15 * MIN)) / 1000.0
    out["bar_close_to_candidate_s"] = {q: float(np.nanquantile(lat, q)) for q in (0.5, 0.9, 0.99)}
    lat_ctx = (ctx.available_at_ms - (ctx.event_time_ms + 15 * MIN)) / 1000.0
    out["bar_close_to_context_s"] = {q: float(np.nanquantile(lat_ctx, q)) for q in (0.5, 0.9, 0.99)}

    rows_ev, rows_c, base_rows = [], [], []
    for iid, g in cdl.sort_values("open_ms").groupby("instrument_id"):
        g = g.drop_duplicates("open_ms")
        t0 = g.open_ms.min()
        n = int((g.open_ms.max() - t0) // MIN) + 1
        close = np.full(n, np.nan)
        idx = ((g.open_ms - t0) // MIN).astype(int).values
        close[idx] = g.close.values
        close_ms = t0 + np.arange(n) * MIN + MIN          # the bar's close time
        lr = np.diff(np.log(close), prepend=np.nan)
        sig = pd.Series(lr).rolling(1440, min_periods=1200).std().values
        cs = cand[cand.canonical_pair_id == iid].copy()
        for h in H:
            fwd = np.full(n, np.nan)
            fwd[:-h] = np.log(close[h:] / close[:-h])
            thr = 2.0 * sig * np.sqrt(h)
            sigf = np.abs(fwd) >= thr
            # greedy non-overlapping events
            i = 0
            starts = []
            while i < n - h:
                if sigf[i] and np.isfinite(fwd[i]):
                    starts.append(i)
                    i += h
                else:
                    i += 1
            for s in starts:
                d = 1 if fwd[s] > 0 else -1
                ts = close_ms[s]
                same = cs[(cs.side == ("LONG" if d > 0 else "SHORT"))]
                pub = same.created_at_ms.values
                early = pub[(pub >= ts - h * MIN) & (pub <= ts)]
                anyw = pub[(pub >= ts - h * MIN) & (pub <= ts + h * MIN // 2)]
                # Null: the same windows filled by the OPPOSITE side. Dense
                # signal streams cover moves by density alone; direction is
                # what shows information.
                opp = cs[(cs.side == ("SHORT" if d > 0 else "LONG"))].created_at_ms.values
                opp_any = opp[(opp >= ts - h * MIN) & (opp <= ts + h * MIN // 2)]
                rows_ev.append(dict(inst=iid, h=h, start_ms=int(ts), dir=d,
                                    covered_early=len(early) > 0, covered_any=len(anyw) > 0,
                                    covered_any_opposite=len(opp_any) > 0,
                                    lead_min=float((ts - early.max()) / MIN) if len(early) else np.nan))
            # base rate: at any minute (every 5th), how often does a random
            # side see a favourable 2-sigma excursion within h?
            win = pd.Series(close)
            hi = win[::-1].rolling(h, min_periods=h).max()[::-1].shift(-1).values
            lo = win[::-1].rolling(h, min_periods=h).min()[::-1].shift(-1).values
            ok = np.isfinite(hi) & np.isfinite(lo) & np.isfinite(sig) & np.isfinite(close)
            sel = np.flatnonzero(ok)[::5]
            unit_b = sig[sel] * np.sqrt(h)
            up = np.log(hi[sel] / close[sel]) >= 2 * unit_b
            dn = -np.log(lo[sel] / close[sel]) >= 2 * unit_b
            base_rows.append(dict(h=h, n=len(sel), fav=float((up.mean() + dn.mean()) / 2)))
        # per-candidate outcome on its own horizon
        for r in cs.itertuples():
            h = horizon_of(r.expected_horizon_ms)
            k = int((r.created_at_ms - t0) // MIN) - 1       # last closed bar before publication
            if k < h or k + h >= n or not np.isfinite(close[k]) or not np.isfinite(sig[k]):
                continue
            d = 1 if r.side == "LONG" else -1
            path = np.log(close[k + 1:k + h + 1] / close[k]) * d
            if np.isnan(path).any():
                continue
            unit = sig[k] * np.sqrt(h)
            fav, adv = path.max(), -path.min()
            first = np.argmax(path >= 2 * unit) + 1 if (path >= 2 * unit).any() else np.nan
            rows_c.append(dict(inst=iid, h=h, strategy=r.strategy_id, fav_u=fav / unit, adv_u=adv / unit,
                               ret_u=path[-1] / unit, hit=fav >= 2 * unit, adverse=adv >= 2 * unit,
                               t_first_min=first,
                               already_u=np.log(close[k] / close[k - h]) * d / unit))
    ev = pd.DataFrame(rows_ev)
    cc = pd.DataFrame(rows_c)
    base = pd.DataFrame(base_rows)
    out["events"] = {}
    for h in H:
        b = base[base.h == h]
        base_rate = float((b.fav * b.n).sum() / b.n.sum())
        e = ev[ev.h == h]
        c = cc[cc.h == h]
        out["events"][h] = {
            "significant_moves": int(len(e)),
            "coverage_early": float(e.covered_early.mean()),
            "coverage_any": float(e.covered_any.mean()),
            "false_negative_rate_any": float(1 - e.covered_any.mean()),
            "coverage_any_opposite_side_null": float(e.covered_any_opposite.mean()),
            "lead_min_median": float(e.lead_min.median()),
            "lead_min_p25_p75": [float(e.lead_min.quantile(0.25)), float(e.lead_min.quantile(0.75))],
            "signals": int(len(c)),
            "hit_rate": float(c.hit.mean()) if len(c) else None,
            "base_rate_random_side": base_rate,
            "adverse_rate": float(c.adverse.mean()) if len(c) else None,
            "false_positive_rate": float(1 - c.hit.mean()) if len(c) else None,
            "t_first_min_median": float(c.t_first_min.median()) if len(c) else None,
            "already_moved_median_u": float(c.already_u.median()) if len(c) else None,
            "ret_mean_u": float(c.ret_u.mean()) if len(c) else None,
        }
    # base rate: how often ANY minute is followed by a favourable 2-sigma excursion in a random direction
    by = cc.groupby(["h", "strategy"]).agg(n=("hit", "size"), hit=("hit", "mean"), adverse=("adverse", "mean"),
                                            already=("already_u", "median"), ret=("ret_u", "mean"))
    out["by_strategy"] = {f"{h}:{s}": {k: float(v) for k, v in row.items()} for (h, s), row in by.iterrows()}
    json.dump(out, open(f"{D}/leadtime.json", "w"), indent=1, default=float)
    print(json.dumps({k: out[k] for k in ("period", "candidates", "instruments", "bar_close_to_candidate_s",
                                          "bar_close_to_context_s")}))
    for h in H:
        print(h, json.dumps(out["events"][h]))
    print(by.round(3).to_string())
    return 0


if __name__ == "__main__":
    sys.exit(main())
