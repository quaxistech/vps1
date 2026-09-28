"""D_long_v1 experiment suite. Every comparison below shares exp.py's protocol:
same rows (unless the variant's point is a different training universe), same
labels, same folds, same cost model, same parameters. Results append to
results.jsonl, one line per (variant, fold).

Usage: DLONG=<blocks dir> python run_all.py <suite> [...]
"""

import json
import os
import sys

import numpy as np

import exp as E

TFA = ["tl_1m", "tl_15m", "tl_1h"]                     # production recipe-1 grids
TFB = TFA + ["tl_4h"]
TFC = TFB + ["tl_1d"]
TF_ALL = ["tl_1m", "tl_5m", "tl_15m", "tl_30m", "tl_1h", "tl_4h", "tl_1d"]
STATE = TF_ALL + ["xtf", "cdl", "age"]                 # candle-only complete state
MAY27 = 1_779_840_000_000                              # macro block starts
JUN27 = 1_782_518_400_000                              # derivatives block starts


def out():
    return open(os.path.join(E.DS, "results.jsonl"), "a")


def suite_baseline(log):
    # production formulation on the long universe: A grids, 3-class; flat 16 bps vs cost v2
    for h in ("15m", "1h", "4h"):
        E.run_variant("base_A_v1cost", TFA, h, "c3", cost="v1", log=log)
        E.run_variant("base_A_v2cost", TFA, h, "c3", cost="v2", log=log)


def suite_tf(log):
    # tfA == base_A_v2cost
    for h in ("1h", "4h"):
        E.run_variant("tfB_4h", TFB, h, "c3", log=log)
        E.run_variant("tfC_4h_1d", TFC, h, "c3", log=log)


def suite_state(log):
    import pickle
    su = E.setup_descriptors()
    for h in ("15m", "1h", "4h"):
        res, preds = E.run_variant("state", STATE, h, "c3", log=log, keep_preds=True, importance=True)
        with open(os.path.join(E.DS, "preds_state_%s.pkl" % h), "wb") as f:
            pickle.dump({"preds": preds, "gain": [r_.get("_gain") for r_ in res]}, f)
        E.run_variant("state_setup", STATE, h, "c3", extra=su, log=log)


def suite_target(log):
    for h in ("15m", "1h", "4h"):
        E.run_variant("tgt_c5", STATE, h, "c5", log=log)
        E.run_variant("tgt_reg", STATE, h, "reg", log=log)


def suite_selection(log):
    import pickle
    su = E.setup_descriptors()
    anyf = su["su_any"]
    for h in ("15m", "1h", "4h"):
        res, preds = E.run_variant("sel_trained_on_setups", STATE, h, "c3", train_filter=anyf, log=log,
                                   keep_preds=True)
        with open(os.path.join(E.DS, "preds_selcand_%s.pkl" % h), "wb") as f:
            pickle.dump({"preds": preds}, f)


def suite_xv(log):
    for h in ("15m", "1h", "4h"):
        E.run_variant("xv1_consensus", STATE + ["xv"], h, "c3", log=log)
        E.run_variant("xv2_consensus_ref", STATE + ["xv", "ref_15m", "ref_1h", "ref_4h"], h, "c3", log=log)


def suite_macro(log):
    for h in ("1h", "4h"):
        kw = dict(start_ms=MAY27, months=("2026-07", "2026-08", "2026-09"), log=log)
        E.run_variant("mac0_state", STATE, h, "c3", **kw)
        E.run_variant("mac1_macro", STATE + ["mac"], h, "c3", **kw)
        arr, cols = E.block("mac")
        a1, c1 = E.block("tl_1h")
        st = np.asarray(a1[:, c1.index("1h:ema_stack")], dtype=np.float64)
        inter = {("macx_" + c): np.asarray(arr[:, i], dtype=np.float64) * st for i, c in enumerate(cols)}
        E.run_variant("mac2_macro_x_regime", STATE + ["mac"], h, "c3", extra=inter, **kw)


SUITES = {k[6:]: v for k, v in globals().items() if k.startswith("suite_")}

if __name__ == "__main__":
    with out() as log:
        for s in sys.argv[1:]:
            SUITES[s](log)
