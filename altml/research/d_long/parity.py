"""Parity: D_long_v1's reconstructed talib-core values against the vectors
AltTech actually published into AltML's archive for the same bars.

A training row is only worth building if it holds the number production would
have seen. Reports, per timeframe and column, the share of bars whose values
agree to 1e-4 relative and the worst relative difference.
"""

import os
import sys

import numpy as np
import pandas as pd


def main(root):
    hdr = open(os.path.join(root, "talib", "header.csv")).read().strip().split(",")
    cols = hdr[4:]
    rows = []
    for tf in ("1m", "15m", "1h", "4h"):
        arch = pd.read_csv(os.path.join(root, "parity", "archive_%s.csv.gz" % tf), header=None,
                           names=["instrument_id", "open_time_ms", "path"] + cols)
        ids = set(arch.instrument_id.unique())
        rec = pd.read_csv(os.path.join(root, "talib", "ex1_%s.csv.gz" % tf), header=None, names=hdr)
        rec = rec[rec.instrument_id.isin(ids) & (rec.status == "VALID")]
        m = arch.merge(rec, on=["instrument_id", "open_time_ms"], suffixes=("_a", "_r"))
        for c in cols:
            a, r = m[c + "_a"].values.astype(float), m[c + "_r"].values.astype(float)
            ok = np.isfinite(a) & np.isfinite(r)
            if ok.sum() == 0:
                rows.append((tf, c, 0, np.nan, np.nan, np.nan))
                continue
            rel = np.abs(a[ok] - r[ok]) / np.maximum(np.abs(a[ok]), 1e-9)
            rows.append((tf, c, int(ok.sum()), float((rel <= 1e-4).mean()),
                         float(np.quantile(rel, 0.99)), float(rel.max())))
        print(tf, "matched bars", len(m), "of archive", len(arch), flush=True)
    out = pd.DataFrame(rows, columns=["tf", "column", "n", "share_1e-4", "rel_p99", "rel_max"])
    out.to_csv(os.path.join(root, "parity_report.csv"), index=False)
    s = out.groupby("tf").agg(n=("n", "median"), share=("share_1e-4", "median"),
                              worst_share=("share_1e-4", "min"), p99=("rel_p99", "median"))
    print(s.to_string())
    print("worst columns:")
    print(out.sort_values("share_1e-4").head(12).to_string(index=False))


if __name__ == "__main__":
    main(sys.argv[1])
