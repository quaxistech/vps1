"""Latency and cost decomposition of the full-state model's OOS decisions.

Reads the saved walk-forward predictions of the `state` variant
(blocks/preds_state_<h>.pkl, written by run_all.py) and, for each horizon,
re-prices the SAME decisions under every entry latency (no refit):

  q1pct   the ranking diagnostic -- extreme 1% of the predicted return, cut on
          the validation distribution (the largest decision set there is)
  q5pct   the same at 5%, cut on the test fold itself (diagnostic only: the
          cut is not point-in-time, the latency effect is)

Latency L is the entry delay after tau in minutes (tau = bar close + 5 s).
`lat_effect` = side * (r_L - r_0) in bps: what the delay did to the trade;
negative = the delay cost money. `unsigned` = |r_L - r_0|: the noise the delay
adds regardless of direction.
"""
import json
import sys

import numpy as np
import pandas as pd

import common as C
import exp as E


def main():
    R = E.rows()
    tau = R.tau_ms.values
    inst = R.instrument_id.values
    cbps = E.cost_col("cost_rt_bps")
    spread = E.cost_col("spread_bps")
    out = {}
    for h in C.HORIZONS:
        hm = {"15m": 15, "1h": 60, "4h": 240}[h]
        P = pd.read_pickle(f"{E.DS}/preds_state_{h}.pkl")["preds"]
        rows_ = P.row.values
        r0 = E.label(f"r_{h}_L0")
        res = {}
        for name, side_full in (("q1pct", None), ("q5pct", None)):
            side = np.zeros(len(R), dtype=int)
            if name == "q1pct":
                side[rows_] = P.sideq.values
            else:
                for f, g in P.groupby("fold"):
                    lo, hi = np.quantile(g.er, 0.05), np.quantile(g.er, 0.95)
                    side[g.row.values] = np.where(g.er >= hi, 1, np.where(g.er <= lo, -1, 0))
            sel = E.select_trades(side, tau, inst, hm * C.MIN_MS)
            by = {}
            for L in C.LATENCIES:
                rl = E.label(f"r_{h}_L{L}")
                ok = sel[np.isfinite(rl[sel]) & np.isfinite(r0[sel])]
                s = side[ok].astype(float)
                e = E.economics(ok, side, rl, r0, cbps, spread, tau, drift_r=rl)
                d = (rl[ok] - r0[ok]) * 1e4
                by[f"L{L}"] = dict(trades=e["trades"], gross=e["gross"], lat_effect=float((s * d).mean()),
                                   unsigned=float(np.abs(d).mean()), fee=e["fee"], spread=e["spread"],
                                   impact=e["impact"], net=e["net"], t_day=e["t_day"], drift=e["drift"],
                                   excess_vs_drift=e["excess_vs_drift"], long_share=e["long_share"])
            res[name] = by
        out[h] = res
    json.dump(out, open(f"{E.DS}/analysis/latency_cost.json", "w"), indent=1)
    for h, res in out.items():
        for name, by in res.items():
            for L, e in by.items():
                print(f"{h:>4} {name} {L}: n={e['trades']:5d} gross={e['gross']:+7.2f} lat={e['lat_effect']:+6.2f} "
                      f"|dL|={e['unsigned']:5.2f} fee={e['fee']:.1f} spr={e['spread']:.2f} imp={e['impact']:.1f} "
                      f"net={e['net']:+7.2f} t={e['t_day']:+5.2f} drift={e['drift']:+6.2f} "
                      f"xs_drift={e['excess_vs_drift']:+6.2f} long={e['long_share']:.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
