"""Stage 0: raw 1m candle snapshot -> one minute-grid array set per (venue, instrument).

The grid runs from 2025-12-01 00:00 UTC to the snapshot cutoff; a minute the
venue did not deliver (the whole of January and March 2026 are absent from
AltSpotter's history) stays NaN / present=False. Nothing is filled.
"""

import os
import sys
import numpy as np
import pandas as pd

from common import CUTOFF_MS, MIN_MS, MONTHS, VENUES

START_MS = 1_764_547_200_000     # 2025-12-01 00:00:00 UTC
N_MIN = (CUTOFF_MS - START_MS) // MIN_MS
COLS = ["id", "t", "o", "h", "l", "c", "bv", "qv", "tb", "ts", "n", "ok"]


def main(root, out):
    os.makedirs(out, exist_ok=True)
    for ex in VENUES:
        per = {}
        def arrays():
            a = {c: np.full(N_MIN, np.nan) for c in ("o", "h", "l", "c")}
            a.update({c: np.full(N_MIN, np.nan, dtype=np.float32) for c in ("bv", "qv", "tb", "ts", "n")})
            a["ok"] = np.zeros(N_MIN, dtype=bool)
            a["present"] = np.zeros(N_MIN, dtype=bool)
            return a
        for m in MONTHS:
            f = os.path.join(root, "raw", "candles_1m_ex%d_%s.csv.gz" % (ex, m))
            df = pd.read_csv(f, header=None, names=COLS,
                             dtype={"id": np.int32, "t": np.int64, "o": np.float64, "h": np.float64,
                                    "l": np.float64, "c": np.float64, "bv": np.float64, "qv": np.float64,
                                    "tb": np.float64, "ts": np.float64, "n": np.float64, "ok": np.int8})
            off = df.t.values - START_MS
            keep = (off >= 0) & (off < N_MIN * MIN_MS) & (off % MIN_MS == 0)
            df = df[keep]
            for iid, g in df.groupby("id", sort=False):
                a = per.setdefault(int(iid), arrays())
                idx = ((g.t.values - START_MS) // MIN_MS).astype(np.int64)
                for c in ("o", "h", "l", "c", "bv", "qv", "tb", "ts", "n"):
                    a[c][idx] = g[c].values
                a["ok"][idx] = g.ok.values.astype(bool)
                a["present"][idx] = True
            del df
        for iid, a in per.items():
            np.savez(os.path.join(out, "m1_ex%d_%d.npz" % (ex, iid)), **a)
        print("venue", ex, "instruments", len(per), flush=True)
        del per


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
