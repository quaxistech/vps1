# VPS1 market-intelligence boundary

```
VPS1 = MARKET INTELLIGENCE / OPPORTUNITY GENERATION
VPS2 = TRADING INTELLIGENCE / OPPORTUNITY SELECTION
```

VPS1 sees the market, detects meaningful events, predicts what the market does
after them over several horizons, and records all of it for learning. It does
**not** decide whether to risk money. That decision — with current prices, the
live book, execution costs, and VPS2's own models and outcomes — belongs to
VPS2.

## 1. Responsibility split

| | VPS1 | VPS2 |
|---|---|---|
| owns | market history (AltSpotter), features (AltTech), macro history (AltMacro), market predictions (AltML), setup/event signals and their **market** outcomes (AltStrategy), signal validity (AltSignalQuality), delivery (AltSignalGateway) | SignalEvaluation, LightGBM + CatBoost ensemble, risk, orders, fills, positions, **execution** outcomes |
| answers | "what is happening, and what does the market usually do next?" | "is it worth acting on, now, at today's costs?" |
| writes | only its own databases | only its own databases |
| reads | the venues | VPS1 **read-only** (below) |

**VPS2 MAY READ VPS1 DATA. VPS2 MUST NOT WRITE VPS1 DATA.**

## 2. What a VPS1 signal is (`vps1-signal-1`)

> At time T a market state was observed that matches a named setup / event,
> with a direction; the model expects the market to move this way, this far,
> over these horizons.

It is **not** "buy now", not a size, not a leverage, not a risk budget.

```
market data ─► market state ─► setup / event ─► ML prediction ─► VPS1 signal
(AltSpotter)   (AltTech)        (AltStrategy)    (AltML, 15m/1h/4h)  (ASQ ─► Gateway)
```

Every VPS1 signal carries:

* the event: `setup_id` (one of the 16 detectors), side, the setup's measured
  descriptors (distance past the level in ATR, volume anomaly, venue agreement,
  room to the next zone, regime per grid, ...), the hypothesis geometry
  (invalidation and target levels — a description, not an order);
* the market view on **every** served horizon (15m, 1h, 4h): `p_up / p_flat /
  p_down`, expected return, expected up and down excursions, the model id and
  role (ACTIVE / SHADOW), whether the probabilities passed the holdout
  calibration test, and the prediction's condition (PRESENT / INCOMPLETE /
  STALE / ABSTAIN / INCOMPATIBLE / MISSING);
* an **economic diagnostic** (AltML's cost model at T0 and the expected net per
  side) — recorded, never a gate;
* lineage: context id, feature manifest hash, model ids, schema versions.

`recommended_for_execution` is **always false**: VPS1 takes no execution
decision. A signal is published whatever any economic estimate says; a signal
that later loses money is normal and is exactly the record learning needs.

What is **not** a signal: every minute's model output on its own. Signals have
market semantics — a detector fired. The model's view on every minute is stored
(AltML `model_predictions`) and reaches the signal as its market view.

## 3. Words that must not be confused

```
market state ≠ setup ≠ prediction ≠ signal ≠ VPS2 decision ≠ risk decision
             ≠ order ≠ fill ≠ position ≠ execution result ≠ profit
```

| term | meaning | where |
|---|---|---|
| market state | the point-in-time feature vector of an instrument | AltTech |
| setup / event | a detector's statement that a named pattern occurred | AltStrategy |
| prediction | the model's view of the market after the state (probabilities, magnitudes) | AltML |
| signal | an event + its market view, published by VPS1 | Gateway |
| VPS2 decision | whether to act on a signal | VPS2 |
| **bid/ask spread** | the cost of liquidity: the distance between best bid and best ask | a cost |
| **large price move** | a significant change of price over a horizon | a market outcome |

"Large spread" never means "large move". VPS2's interest is a *large enough
expected move relative to the actual execution cost*.

| outcome | contents | owner |
|---|---|---|
| **market outcome** | return over the horizon, MFE, MAE, time to MFE / MAE | VPS1 (`candidate_market_outcomes`) |
| **execution outcome** | order, fill, slippage, fees, funding, position, realised PnL, exit reason | VPS2 |

## 4. Failure semantics (fail closed on the prediction, never on the event)

| condition | effect |
|---|---|
| no model / bad model / stale model | no model prediction; the horizon reads MISSING / INCOMPATIBLE / STALE; the signal is still published, graded WEAK |
| model inputs partly missing | the horizon reads INCOMPLETE (numbers kept, condition stated) |
| bad or expired context, invalid geometry | the signal is INVALID and never published |
| missing mandatory feature | the setup is not evaluated (`missing_feature`) |
| future data | impossible by construction: every input is read as of T0 (docs/vps1/TEMPORAL_MODEL.md; the book spread by arrival order) |
| VPS2 absent | nothing in VPS1 waits for, reads from or depends on VPS2 |

`model prediction ≠ economic approval`: a positive expected net approves
nothing, a negative one suppresses nothing.

## 5. History (what is kept per signal)

| field | table |
|---|---|
| signal id, timestamp, instrument, direction, setup, plan geometry | `altstrategy.trade_signal_proposals`, `altsignalgateway.signal_journal` |
| all relevant features at T0 | `altstrategy.decision_context_snapshots` (by `context_id`) |
| setup descriptors | `altstrategy.strategy_candidates.descriptors` |
| model id / version, feature schema, prediction per horizon, expected magnitude | `altstrategy.candidate_predictions`; AltML `model_predictions` for every minute |
| market outcome per horizon: return, MFE, MAE, time to MFE / MAE | `altstrategy.candidate_market_outcomes` |

Every candidate — published or not, whatever its grade — gets its market
outcome, so false positives and the market after every event are recorded.

## 6. VPS2 read-only access

* VPS2 reads approved AltSpotter market tables and instrument constraints
  through read-only PostgreSQL access (`postgres_fdw`). Signal delivery and
  signal-journal queries use the AltSignalGateway protocol.
* VPS1 remains authoritative for AltSpotter market history, AltTech features,
  signal history, market outcomes, and AltMacro history. VPS2 may retain the
  records it needs for its own evaluation and execution, but does not write to
  VPS1 data.
* VPS2 has no write path into VPS1 and does not own acknowledgements for
  VPS1-owned NATS consumers.
* VPS1 does not depend on VPS2: the Gateway journals whether or not a client is
  connected, and delivery resumes from a durable cursor.

## 7. Control questions

| question | answer | why |
|---|---|---|
| Can VPS1 keep a signal that later loses money? | **YES** | publication does not depend on any economic estimate; the loss is recorded as its market outcome |
| Can VPS2 see such a signal and reject it? | **YES** | every signal reaches the Gateway with `recommended_for_execution = false`; selection is VPS2's |
| Can VPS2 read VPS1 market context? | **YES**, for approved tables through read-only access (§6) | `postgres_fdw`; signal history uses the Gateway protocol |
| Can VPS2 change VPS1? | **NO** | no write path exists; VPS2 is a TLS client of the Gateway |
| Can the absence of VPS2 stop VPS1? | **NO** | VPS1 journals without a client; nothing upstream reads VPS2 |
