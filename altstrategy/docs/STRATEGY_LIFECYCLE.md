# Strategy lifecycle and publication authority

```
RESEARCH → BACKTESTED → SHADOW → CANDIDATE → LIMITED → ACTIVE
                                                   ↘ DEGRADED → DISABLED → RETIRED
```

Compiled `default_lifecycle` seeds are overridden by deployment rows in
`altstrategy.strategy_lifecycle`, loaded at startup. Neither a seed nor a
lifecycle name proves a current runtime state or economic edge.

| State | Processing | Publication authority |
|---|---|---|
| RESEARCH / SHADOW | eligible candidates run as explore_shadow | technically valid proposals publish; recommendation remains false |
| CANDIDATE / LIMITED / ACTIVE | eligible candidates run as exploit | technically valid proposals publish; recommendation remains false |
| DEGRADED | candidates remain observable | no financial authorization |
| DISABLED / RETIRED | strategy does not run | no candidates |

`recommended_for_execution=false` on every proposal. Economic verdicts are
`ECON_DIAG_*` warnings, not a publication veto. The lifecycle does not grade or
suppress otherwise technically valid events; VPS2 independently decides whether
and how an opportunity may trade. Research candidates are archived and mature
through the same market-outcome tracker, independent of fills or account capacity.

`strategy_drift_snapshots` records outcome drift and the continuous scan can
transition a strategy to DEGRADED. It considers uncertainty/sample evidence,
not raw change alone. Adding a strategy requires a documented hypothesis,
registered implementation and explicit lifecycle contract; tests of deterministic
behavior do not prove the market hypothesis.

[Research workflow](STRATEGY_RESEARCH.md) describes evaluation boundaries.
[Dated lifecycle evidence](archive/lifecycle-observation-2026-10-15.md) is retained
for audit, not current activation. Runtime is **NOT VERIFIED DURING
DOCUMENTATION-ONLY AUDIT**.
