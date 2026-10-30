# The FINAL TradeSignal contract

The canonical, versioned contract an external consumer receives.

```
schema_version    3     the shape of SignalPayload — what a signal IS
protocol_version  2     the framing that carries it — how a signal TRAVELS
```

**What a signal is (schema 3, `vps1-signal-1`).** The payload combines an
AltStrategy market setup with available AltML market forecasts and the
point-in-time context required by a downstream consumer. It is **not** an
execution decision: `recommended_for_execution` is false on every signal, and
no economic estimate decides whether a signal exists. The consumer decides
whether to act using its own current execution conditions.

The two are independent. A new payload field bumps the schema version and leaves
the protocol untouched; a new frame type bumps the protocol version and leaves
every journalled payload readable.

Machine-readable form: `proto/gateway.proto`, plus the descriptor set shipped
with every release at `share/altsignalgateway/proto/altsignalgateway_schema.desc`
— an external consumer can generate bindings from that alone.

## The self-sufficiency rule

> A consumer must be able to act on a signal without querying AltSpotter,
> AltTech, AltML, AltStrategy or AltSignalQuality.

Everything below exists because of that sentence, and nothing exists that does
not serve it. Two fields are carried purely because the upstream wire message
cannot be executed without them:

* **`trade_plan.symbol`** — the upstream `Envelope.symbol` is an internal
  instrument token; this field carries the venue-facing symbol resolved for
  the trade plan.
* **`execution_context.*`** — tick size, lot step and minimum notional. Without
  them a consumer cannot construct a valid order and would have to query VPS1's
  database, which is exactly what this contract exists to avoid.

## The absence rule

> An upstream that encodes "absent" as `0.0` must not become a downstream that
> reads `0.0` as a measurement.

Every optional value is presence-gated. A `has_*` companion, a `present` flag on
a whole block, or both. When a flag is false the value stays at its protobuf
default and **means nothing**. This is not defensive style: a `0.0` spread and
an unknown spread are different markets, a `0.0` ML uncertainty and an absent
model are different situations, and a price of zero is something a consumer can
act on.

## The point-in-time rule

> Nothing that came into existence after T0 may describe the decision snapshot.

`T0 = trade_plan`'s `proposal.created_at_ms`, carried as
`provenance.signal_created_at_ms`. Every field is classified below as one of:

| class | meaning |
|---|---|
| **AT-T0** | describes the state the decision was taken on. Must not reflect anything later. |
| **DECISION** | produced by the decision itself, at or immediately after T0. |
| **TRANSPORT** | produced by this service when it journalled or transmitted the signal. Legitimately after T0. |

The rule is enforced by construction: the market snapshot is read by
`context_id` from an **immutable** row, and the venue spec is selected
`WHERE valid_from <= T0`. Neither read can see a later value.

---

# Envelope

Transport identity and integrity. Contains no trade content, so a router or an
audit tool can read it without parsing the payload.

| field | type | req | class | meaning | unit |
|---|---|---|---|---|---|
| `schema_version` | uint32 | yes | TRANSPORT | shape of `SignalPayload` | — |
| `protocol_version` | uint32 | yes | TRANSPORT | framing that carried it | — |
| `message_id` | string | yes | TRANSPORT | `asgw_` + 32 hex, **deterministic** in `signal_id` | — |
| `signal_id` | string | yes | DECISION | AltSignalQuality's `final_signal_id`; the idempotency key | — |
| `sequence` | int64 | yes | TRANSPORT | gapless, strictly increasing, durable position in the journal | — |
| `signal_type` | string | yes | TRANSPORT | `"FINAL_TRADE_SIGNAL"` | — |
| `created_at_ms` | int64 | yes | AT-T0 | **T0** — the information cut-off | ms UTC |
| `expires_at_ms` | int64 | yes | DECISION | `proposal.valid_until_ms`; after this it is never delivered as live | ms UTC |
| `journaled_at_ms` | int64 | yes | TRANSPORT | when this signal became durable | ms UTC |
| `published_at_ms` | int64 | yes | TRANSPORT | when **this transmission** was serialized | ms UTC |
| `payload_sha256` | string | yes | TRANSPORT | sha256 over the deterministic serialization of `payload` | 64 hex |
| `producer_service` | string | yes | TRANSPORT | `"altsignalgateway"` | — |
| `producer_version` | string | yes | TRANSPORT | this build | — |
| `replay` | bool | yes | TRANSPORT | true when delivered by RESUME rather than live. The payload is identical either way. | — |
| `delivery_attempt` | uint32 | yes | TRANSPORT | 1 on first transmission | — |

**Verifying the hash.** Serialize `payload` deterministically (protobuf
deterministic mode) and sha256 it; the result must equal `payload_sha256`. The
contract contains **no map fields anywhere** precisely so this is reproducible
on any runtime — repeated `NamedValue` entries sorted by `name` are canonical by
construction where a `map<string,double>` would not be. The reference client
performs this check before doing anything else with a trade
(`cli/client.cpp`), and a unit test asserts a `SignalPayload` round-trips to
identical bytes.

---

# trade_plan

The executable trade, copied from AltStrategy and unchanged.

| field | type | req | class | meaning | unit |
|---|---|---|---|---|---|
| `exchange` | string | yes | AT-T0 | AltSpotter exchange code, e.g. `bitget` | — |
| `symbol` | string | yes¹ | AT-T0 | **venue-executable** symbol, e.g. `XRPUSDT` | — |
| `canonical_pair_id` | int64 | yes | DECISION | AltSpotter cross-venue instrument id | — |
| `base_asset` / `quote_asset` | string | yes¹ | AT-T0 | e.g. `XRP` / `USDT` | — |
| `market_type` | string | yes¹ | AT-T0 | `usdt_perp` | — |
| `side` | `altstrategy.v1.Side` | yes | DECISION | `SIDE_LONG` \| `SIDE_SHORT` | — |
| `entry_type` | `altstrategy.v1.EntryType` | yes | DECISION | MARKET \| LIMIT \| STOP_MARKET \| STOP_LIMIT | — |
| `entry_price` | double | yes | DECISION | for MARKET this is the **reference price the geometry was computed against**, not an order price | quote |
| `has_entry_range` + `entry_lower`/`entry_upper` | bool + double | opt | DECISION | acceptable entry band | quote |
| `has_trigger_price` + `trigger_price` | bool + double | opt | DECISION | stop-entry trigger | quote |
| `max_slippage_bps` | double | yes | DECISION | plan's own slippage tolerance | bps |
| `post_only_allowed` | bool | yes | DECISION | | — |
| `expected_fill_time_p50_min` / `_p90_min` | double | yes | DECISION | | minutes |
| `entry_fill_timeout_ms` | int64 | yes | DECISION | abandon an unfilled entry after this | ms |
| `stop_loss_price` | double | yes | DECISION | | quote |
| `stop_basis` | string | opt | DECISION | free-text provenance of the level | — |
| `stop_distance_bps` / `stop_distance_atr` | double | yes | DECISION | | bps / ATR multiples |
| `take_profit_price` | double | yes | DECISION | | quote |
| `has_secondary_target` + `secondary_target_price` | bool + double | opt | DECISION | | quote |
| `target_basis` | string | opt | DECISION | | — |
| `target_distance_bps` | double | yes | DECISION | | bps |
| `expected_rr` | double | yes | DECISION | reward/risk of the plan geometry | ratio |
| `holding_class` | `altstrategy.v1.HoldingClass` | yes | DECISION | SCALP \| INTRADAY \| EXTENDED_INTRADAY | — |
| `expected_holding_time_p50_ms` / `_p90_ms` | int64 | yes | DECISION | | ms |
| `max_holding_time_ms` | int64 | yes | DECISION | hard bound, ≤ 48 h | ms |
| `time_stop_at_ms` | int64 | yes | DECISION | absolute time stop | ms UTC |
| `valid_until_ms` | int64 | yes | DECISION | plan validity; equals `envelope.expires_at_ms` | ms UTC |
| `strategy_id` / `strategy_version` | string | yes | DECISION | repeated here so a trade can be routed without reading another block | — |

¹ Empty when `execution_context.present` is false and its `absence_reason` says
why. A consumer must treat an empty `symbol` as undeliverable rather than
guessing.

**Validated at ingest.** A signal is refused — journalled nowhere, recorded in
`rejected_messages` — unless: prices are finite and positive; long ⇒
`stop < entry < target` and short ⇒ `target < entry < stop` (AltStrategy's own
`chk_long_geometry`/`chk_short_geometry`); `valid_until_ms > created_at_ms`;
`0 < max_holding_time_ms ≤ 48 h` (its `chk_holding_limit`); the entry range is
either fully absent or well-ordered; the grade is not `GRADE_INVALID`.

---

# strategy

| field | type | req | class | meaning |
|---|---|---|---|---|
| `strategy_id`, `strategy_version`, `parameter_set_id` | string | yes | DECISION | which strategy, which version, which parameters |
| `candidate_id`, `ranking_id`, `ranking_policy_id` | string | yes | DECISION | lineage into AltStrategy's own archive |
| `proposal_grade` | `altstrategy.v1.ProposalGrade` | yes | DECISION | STRONG \| NORMAL \| WEAK \| EXPERIMENTAL \| SHADOW |
| `state` | `altstrategy.v1.ProposalState` | yes | DECISION | normally `STATE_PROPOSED` |
| `plan_version` | int32 | yes | DECISION | |
| `recommended_for_execution` | bool | yes | DECISION | **Always false since schema 3**: VPS1 takes no execution decision. Transmitted so a consumer never infers one from the mere presence of the signal. |
| `strategy_confidence`, `ensemble_agreement`, `adaptive_score` | double | yes | DECISION | |
| `context_quality` | `altstrategy.v1.ContextQuality` | yes | AT-T0 | how many venues agreed at T0 |
| `has_outcome_probabilities` + `p_*` | bool + double | opt | DECISION | entry/target/stop/timeout/ambiguous. **Gated:** an all-zero vector is an unpopulated proposal, not a forecast that nothing can happen. |
| `has_expected_returns` + `expected_*` | bool + double | opt | DECISION | gross, net, utility. Same gate. |
| `reason_codes`, `risk_flags` | repeated string | opt | DECISION | machine-readable |
| `warnings` | repeated `altstrategy.v1.ProposalWarning` | opt | DECISION | code / detail / severity |

---

# altml

AltML's contribution, or an explicit statement that there was none.

| field | type | req | class | meaning |
|---|---|---|---|---|
| `present` | bool | yes | — | **the only gate**; everything else is meaningless when false |
| `absence_reason` | string | when absent | — | e.g. `NO_MODEL_VERSIONS_ON_PROPOSAL` |
| `model_versions` | repeated string | when present | DECISION | the models that contributed |
| `abstain` | bool | when present | DECISION | the model declined to forecast |
| `conviction` | double | when present | DECISION | AltStrategy's `ml_conviction`, see below |

Tags 5 and 6 (`uncertainty`, `ood_score`) are reserved: nothing ever produced
them, and both were structurally zero on every signal.

**The presence test is `model_versions` being non-empty** — AltStrategy's own
record of a forecast having been attached to this plan. A proposal without one
carries protobuf defaults, not measurements; the block reports `present=false`
instead.

**What `conviction` is (verified in code, 2026-10-15).** For a proposal with an
econ evaluation, AltStrategy sets `ml_conviction = p_side − p_opposite` of the
econ estimate: signed, in [−1, 1], positive when the model favours the
proposal's own side; its magnitude is AltML's own `confidence`,
`|p_long − p_short|`. The comments on `gateway.proto` tag 7 and on
`decision.proto` tag 78 still describe it as `|p_up − 0.5| · 2` -- the retired
binary model's definition, non-negative. Correcting them changes the files VPS2
vendors and pins by sha256 (`altsignal-intelligence` reads this checkout through
`/home/quaxis/projects/altsignalgateway`), so it is done together with VPS2's
re-vendoring; no wire byte changes.

As of 2026-10-15 (registry, read-only) AltML serves ACTIVE econ models for
15m, 1h and 4h. None has `probability_calibrated = true`.

## Notes for the consumer (VPS2), 2026-10-15

Read from VPS2's source, not changed there:

- `altml_conviction` is declared in [0, 1] by `altsignal-intelligence`
  (`feature_schema.cpp`), and a value outside its domain becomes missing
  (`OUT_OF_RANGE`). Half of all econ proposals carry a negative conviction --
  34 % of LONGs, 64 % of SHORTs (2026-10-01..14) -- so the feature is missing
  on half the signals, lopsidedly the SHORTs. The fix is VPS2's: domain
  [−1, 1] and the signed definition, a new feature-schema version.
- Plan geometry 2 moves the plan features VPS2 reads, on purpose (the same
  60 615 proposals, recorded geometry → geometry 2, p10 / p50 / p90):
  `target_distance_bps` 0.0 / 59.6 / 249.9 → 24.6 / 83.7 / 250.9;
  `stop_distance_bps` 18.2 / 47.3 / 94.9 → 21.3 / 55.7 / 114.0; `expected_rr`
  0.00 / 1.41 / 4.64 → 0.59 / 1.49 / 4.42; VPS2's `stop_to_target_ratio` was
  above its 1000 domain (missing) on 22.4 % and is never now. Targets below the
  estimated cost: 31.4 % → 3.1 % (those carry `PLAN_TARGET_BELOW_COST`, info).
  Anything VPS2 fitted on signals before the cut-over learnt the old geometry.
- Warnings reach VPS2 only as a count (`strategy_warning_count`); the new codes
  `PLAN_TARGET_BELOW_COST` and `TICK_SIZE_DERIVED` cannot be refused there.
- `signal_quality.missing_feature_count` is unchanged in meaning; the reason
  `MISSING_FEATURES` is now raised only for an undeclared gap
  (AltSignalQuality's FEATURE_CONTRACT.md). VPS2 reads the count, not the reason.
- No market outcome travels on this contract: AltStrategy's `MarketOutcome`
  (evaluator 2, a virtual path outcome) stays inside VPS1, apart from anything
  VPS2 measures or executes.
- A redelivered signal keeps its `signal_id` and gets no new sequence here; a
  same `signal_id` with different upstream bytes is an integrity violation,
  kept first and refused loudly. An AltSignalQuality re-publish after a crash
  later than its 2 h de-duplication window would be such a violation (its
  envelope carries a fresh `published_at_ms`), never a second delivery.

---

# signal_quality

AltSignalQuality's verdict, forwarded verbatim.

| field | type | req | class | meaning |
|---|---|---|---|---|
| `evaluation_id` | string | yes | DECISION | |
| `decision` | `altsignalquality.v1.QualityDecision` | yes | DECISION | always `DECISION_ACCEPTED` here — a REJECTED signal is refused at ingest |
| `basis` | `altsignalquality.v1.DecisionBasis` | yes | DECISION | MODEL \| COLD_START \| MODEL_UNUSABLE \| … — **a score-based accept and a cold-start pass-through are different events** |
| `has_quality_score` | bool | yes | — | false ⇒ score, raw score and threshold are meaningless |
| `quality_score` | double | when scored | DECISION | calibrated P(positive cost-adjusted outcome), [0,1] |
| `raw_score` | double | when scored | DECISION | pre-calibration model output |
| `decision_threshold` | double | when scored | DECISION | the fitted threshold it was compared against |
| `reason_codes` | repeated string | opt | DECISION | |
| `model_id`, `model_version` | string, int32 | opt | DECISION | empty on a cold-start decision |
| `feature_schema_version`, `feature_schema_hash` | string | yes | DECISION | what the score is interpretable against |
| `calibration_version`, `label_spec_version` | string | opt | DECISION | |
| `feature_count`, `missing_feature_count` | int32 | yes | DECISION | how complete the vector was |
| `evaluated_at_ms` | int64 | yes | DECISION | ms UTC |
| `inference_latency_us` | int64 | yes | TRANSPORT | µs |

When `has_quality_score` is false the numeric fields are left at their defaults
rather than carrying the upstream's meaningless value — a consumer sizing on a
forwarded `0.5` would be sizing on a protobuf default.

---

# market_snapshot

The venue state **the decision was taken on**, from AltStrategy's immutable
`decision_context_snapshots` row addressed by `provenance.context_id`.

It is not a reading taken when the Gateway journalled the signal. That would be
a different market, minutes later.

| field | type | req | class | meaning | unit |
|---|---|---|---|---|---|
| `present` | bool | yes | — | false ⇒ everything below is absent | |
| `absence_reason` | string | when absent | — | `CONTEXT_SNAPSHOT_NOT_FOUND`, `CONTEXT_REVISION_MISMATCH`, `NO_CONTEXT_ID_ON_SIGNAL`, `CONTEXT_ANALYTICS_UNPARSEABLE` | |
| `event_time_ms` | int64 | when present | AT-T0 | the context's own bar time | ms UTC |
| `available_at_ms` | int64 | when present | AT-T0 | when the context became usable | ms UTC |
| `expires_at_ms` | int64 | when present | AT-T0 | the context's validity bound | ms UTC |
| `context_revision` | int64 | when present | AT-T0 | matched against `proposal.context_revision` | |
| `has_mark_price` + `mark_price` | bool + double | opt | AT-T0 | | quote |
| `has_spread_bps` + `spread_bps` | bool + double | opt | AT-T0 | | bps |
| `has_funding_rate` + `funding_rate` | bool + double | opt | AT-T0 | | fraction |
| `has_fees_bps` + `fees_bps` | bool + double | opt | AT-T0 | round-trip fee assumption | bps |
| `has_top_depth_usdt` + `top_depth_usdt` | bool + double | opt | AT-T0 | | USDT |
| `has_liquidity_rank` + `liquidity_rank` | bool + double | opt | AT-T0 | | rank |
| `has_expected_slippage_bps` + `expected_slippage_bps` | bool + double | opt | AT-T0 | | bps |
| `venue_features` | repeated `NamedValue` | opt | AT-T0 | the named analytics the strategies read (rsi_14, atr_14, adx_14, bb_*, donchian_*, ema_*, …), **sorted by name** | per-metric |
| `cross_venue_features` | repeated `NamedValue` | opt | AT-T0 | dislocation_bps, consensus_*, venue_price_dispersion_bps, funding_rate_8h, … | per-metric |
| `reference_features` | repeated `NamedValue` | opt | AT-T0 | reference-venue analytics | per-metric |
| `global_universe_status` | string | when present | AT-T0 | `ELIGIBLE_ALL_VENUES` \| `MISSING_ON_*` \| … | |
| `data_quality` | `altstrategy.v1.ContextQuality` | when present | AT-T0 | STRICT_4_OF_4 \| DEGRADED_3_OF_4 \| … | |
| `venue_mask`, `missing_venue_mask` | uint32 | when present | AT-T0 | which venues contributed | bitmask |

**Revision matching.** The snapshot is loaded by `context_id` and its
`context_revision` is compared to the proposal's. A mismatch describes a
different market, so the block is reported absent with
`CONTEXT_REVISION_MISMATCH` rather than transmitted.

A non-finite analytic is **dropped**, never transmitted: a NaN that reaches a
consumer as a metric compares false against every bound.

---

# microstructure

Order-flow state at T0, from the same snapshot. The Gateway owns no order book
and computes nothing here.

| field | type | req | class | meaning |
|---|---|---|---|---|
| `present` | bool | yes | — | |
| `absence_reason` | string | when absent | — | `NO_MICROSTRUCTURE_IN_CONTEXT`, or the context's own reason |
| `event_time_ms` | int64 | when present | AT-T0 | ms UTC |
| `values` | repeated `NamedValue` | when present | AT-T0 | `cvd`, `cvd_delta`, `signed_volume`, `trade_intensity`, sorted by name |

**What is deliberately not here.** Raw order-book levels, per-level depth and
OFI are not in AltStrategy's persisted snapshot, so this service cannot carry
them without recomputing them from the market archive — which would be a reading
taken *now*, not at T0, and would make the Gateway a market-data component. See
"Remaining limitations" in the README.

---

# regime

| field | type | req | class | meaning |
|---|---|---|---|---|
| `present` | bool | yes | — | |
| `absence_reason` | string | when absent | — | `NO_REGIME_IN_CONTEXT` or the context's own reason |
| `venue_regime` | `RegimeView` | when present | AT-T0 | the execution venue's regime |
| `global_regime` | `RegimeView` | when present | AT-T0 | the cross-venue consensus regime |
| `regime_divergence` | bool | when present | AT-T0 | the two disagreed |

`RegimeView`: `regime` (a name such as `RANGE`, `TREND_DOWN`) plus four
presence-gated scalars — `confidence`, `trend_strength`, `volatility_regime`,
`liquidity_quality`.

---

# execution_context

The venue's own trading constraints, resolved from AltSpotter's registry **as of
T0**. It exists so an execution layer never has to query VPS1 to learn what a
valid order looks like. It is not a trading instruction: no size, no leverage
choice, no account state.

| field | type | req | class | meaning |
|---|---|---|---|---|
| `present` | bool | yes | — | true only when both the instrument **and** its spec resolved |
| `absence_reason` | string | when absent | — | `NO_VENUE_MAPPING_FOR_CANONICAL_PAIR`, `NO_INSTRUMENT_SPEC_AS_OF_SIGNAL_TIME`, `INSTRUMENT_SPEC_UNPARSEABLE` |
| `exchange` | string | yes | AT-T0 | |
| `exchange_instrument_id` | int64 | yes¹ | AT-T0 | AltSpotter `exchange_instruments.id` |
| `exchange_symbol` | string | yes¹ | AT-T0 | same value as `trade_plan.symbol` |
| `instrument_status` | string | yes¹ | AT-T0 | `active` \| `suspended` \| `delisted` — **check this before ordering** |
| `whitelisted` | bool | yes¹ | AT-T0 | |
| `spec_version`, `spec_valid_from_ms` | int64 | when present | AT-T0 | which spec revision, and when it took effect |
| `contract_type`, `settle_asset` | string | when present | AT-T0 | `perpetual`, `USDT` |
| `tick_size` | **string** | when present | AT-T0 | exact decimal, e.g. `"0.0001"` |
| `quantity_step` | **string** | when present | AT-T0 | exact decimal |
| `min_order_quantity` | **string** | when present | AT-T0 | exact decimal |
| `min_notional` | **string** | when present | AT-T0 | exact decimal, quote currency |
| `contract_value` | **string** | when present | AT-T0 | exact decimal |
| `has_price_precision` + `price_precision` | bool + int32 | opt | AT-T0 | decimal places |
| `has_quantity_precision` + `quantity_precision` | bool + int32 | opt | AT-T0 | decimal places |
| `has_max_leverage` + `max_leverage` | bool + double | opt | AT-T0 | venue maximum, **not** a sizing instruction |
| `resolved_at_ms` | int64 | yes | TRANSPORT | when the lookup ran | 

¹ Populated whenever the instrument resolved, even if the spec did not — those
are different failures with different consequences, and a symbol whose tick size
is unknown is more useful than no symbol at all.

**Why the decimals are strings.** `0.0001` as an IEEE double is
`0.000100000000000000004792…`. Rounding a price to that produces an order the
venue rejects for an invalid tick. The registry stores them as exact decimal
strings and they are carried as such, end to end.

**Point-in-time selection.** The spec is the version in force at T0
(`WHERE valid_from <= T0 ORDER BY valid_from DESC LIMIT 1`), not the newest one:
the plan's prices were computed against the venue's rules as they stood when the
decision was taken. `spec_valid_from_ms` lets a consumer see exactly which
revision it received.

---

# macro  (schema_version 2)

AltTech's macro-core block as of T0: forty measured cross-asset quantities
(CNY/CNH against the PBOC parity and the H.10 reference, the majors and gold,
VIX and the US index closes, the Treasury curve), one vector per minute for
the whole system, read from AltTech's own table over a read-only foreign
connection at ingest. Names, units, source times and freshness bounds are part
of the versioned `MacroSnapshot` message in `proto/gateway.proto`.

Both the bar time and computation time must be at or before the original T0.
If a later method/source correction replaced that minute, the reader can use
an older eligible minute within the age bound or report absence; it never
presents the replacement as information available before its computation.
Already journalled payloads remain immutable.

| field | type | req | class | meaning |
|---|---|---|---|---|
| `present` | bool | yes | — | false ⇒ everything below is absent |
| `absence_reason` | string | when absent | — | `NO_MACRO_VECTOR_WITHIN_MAX_AGE`, `MACRO_VECTOR_WARMING`, `MACRO_WIDTH_MISMATCH`, `MACRO_READ_FAILED` |
| `feature_set_name` / `feature_set_version` / `schema_hash` | string / int32 / string | when present | — | the producer's frozen registry entry (`macro-core`, 1) |
| `method_version` | string | when present | — | AltTech's computation + availability model, e.g. `mc-1.2.0` |
| `bar_open_time_ms`, `computed_at_ms`, `age_ms` | int64 | when present | AT-T0 | the minute described, when it was computed, `T0 − bar_open_time_ms` |
| `status` | string | when present | — | `VALID` \| `INCOMPLETE_INPUT` (partial validity is normal: weekends, daily sources) |
| `feature_count`, `valid_count` | int32 | when present | — | 40, and how many were usable |
| `values[]` | NamedValue | when present | AT-T0 | usable positions, sorted by name; never 0.0 for a missing reading |
| `unavailable[]` | string | when present | — | the rest, by name, sorted |
| `source_service` | string | when present | — | `alttech` |

Rows journalled before schema 2 have no macro block at all (`present` reads
false, no reason) and are served with `envelope.schema_version = 1`.

# market_forecast  (schema_version 3)

AltML's market prediction on every served horizon, as the signal carried it at
T0, in the LONG perspective (the signal's side is on `trade_plan.side`).

| field | type | req | class | meaning |
|---|---|---|---|---|
| `present` | bool | yes | — | false ⇒ the proposal carried no market view at all (`absence_reason`) |
| `evaluation_horizon_minutes` | int32 | when present | DECISION | the horizon matching the setup's own holding horizon |
| `signal_semantics` | string | when present | — | `vps1-signal-1` |
| `horizons[]` | HorizonForecast | when present | AT-T0 | ascending horizon; always every served horizon (15, 60, 240) |

`HorizonForecast`:

| field | type | meaning |
|---|---|---|
| `horizon_minutes` | int32 | 15, 60 or 240 |
| `state` | string | `PRESENT`, `INCOMPLETE` (some model inputs missing), `STALE`, `ABSTAIN`, `INCOMPATIBLE`, `MISSING`, or `INCOHERENT_VALUES` (refused here) |
| `present` | bool | **the gate**: false ⇒ every number below is absent (protobuf default, not a measurement) |
| `model_id`, `model_version`, `model_role` | | the model; `ACTIVE` passed AltML's market-quality gates, `SHADOW` did not |
| `calibration_version`, `probability_calibrated` | | the probabilities are probabilities only when `probability_calibrated` (they passed the holdout calibration test); otherwise calibrated-by-intent scores |
| `p_up`, `p_flat`, `p_down` | double | what up / flat / down mean is the model's label definition (AltML registry) |
| `expected_return_bps` | double | expected simple return over the horizon |
| `expected_up_excursion_bps`, `expected_down_excursion_bps` | double | expected maximum move up / down over the horizon, magnitudes ≥ 0 |
| `bar_open_time_ms`, `computed_at_ms`, `age_ms` | int64 | the 1m bar predicted on, when, and `T0 − close of that bar` |
| `aligned_missing` | int32 | model inputs absent at scoring |
| `cost_model_version`, `cost_bps`, `expected_net_long_bps`, `expected_net_short_bps` | | **economic DIAGNOSTIC** from AltML's cost model at T0; VPS1 gates nothing on it |

# setup  (schema_version 3)

| field | type | meaning |
|---|---|---|
| `setup_id` | string | the detector (== `strategy_id`) |
| `descriptors[]` | NamedValue | named setup context values, sorted by name; values describe the setup and do not change its definition |

# provenance

Everything needed to reproduce, audit or dispute one signal without access to
any VPS1 service.

**Identities** — `signal_proposal_id` (AltStrategy's idempotency key),
`candidate_id`, `ranking_id`, `final_signal_id`, `evaluation_id`, `context_id`,
`context_revision`.

**Model and schema lineage** — `feature_manifest_hash` (AltTech/AltStrategy),
`quality_feature_schema_version` and `_hash`, `quality_model_id` and
`_version`, `quality_calibration_version`, `quality_label_spec_version`,
`altml_model_versions`, `strategy_id`, `strategy_version`, `parameter_set_id`,
`ranking_policy_id`.

**Timestamps**, all UTC milliseconds and all classified:

| field | class |
|---|---|
| `signal_created_at_ms` | **T0** |
| `context_event_time_ms`, `context_available_at_ms` | AT-T0 (≤ T0) |
| `quality_evaluated_at_ms`, `quality_published_at_ms` | DECISION |
| `gateway_received_at_ms`, `gateway_journaled_at_ms` | TRANSPORT |

**Producers** — `altsignalquality_version`, `altsignalgateway_version`.

**Integrity of the upstream payload** — `source_envelope_sha256` (sha256 over
`payload.source_envelope`), `source_subject`, `source_stream`,
`source_stream_sequence`.

---

# source_envelope

`SignalPayload.source_envelope` carries the **exact serialized
`altsignalquality.v1.Envelope`, byte for byte as it arrived on NATS**.

It is carried and not merely hashed for one reason: a consumer must be able to
prove the canonical projection above against the producer's own message without
asking VPS1 for it. Everything else in the payload is derivable from these bytes
plus the two registry lookups.

To verify:

```
sha256(payload.source_envelope) == payload.provenance.source_envelope_sha256
parse it as altsignalquality.v1.Envelope
compare .final_trade_signal.proposal field by field against trade_plan
```

`altsignalgateway verify` performs exactly this check against the journal, plus
the canonical-stability check, over the whole retained history.

Cost: roughly 1.3 KB per signal. See [JOURNAL.md](JOURNAL.md) for the arithmetic.

---

# Immutability

A journalled signal is a historical fact. Its identity and both payloads cannot
be changed — enforced by a PostgreSQL trigger, not merely by convention. A
corrected trade is a **new signal** with its own `signal_id`, published
upstream, arriving here as a new row.

The one condition that is never resolved by overwriting: the same `signal_id`
arriving with **different upstream bytes**. One of the two is the trade AltStrategy
decided on and this service cannot know which, so it keeps the first, refuses
the second, records both hashes in `integrity_violations` and logs at critical.

# Versioning

* A field added to an existing block: **compatible**, no version bump — protobuf
  ignores unknown fields, and the hash is computed by both sides over what they
  serialize.
* A field whose meaning or unit changes, or a new block: a schema version bump.
  `HELLO` carries `supported_schema_versions`; the server refuses a client that
  does not advertise the schema it emits. This build emits schema 3. Older
  schema-1 and schema-2 journal rows remain readable and retain their original
  serialized bytes and hashes.
* Field numbers on `gateway.proto` are **frozen**; `tests/unit/test_wire_contract.cpp`
  asserts each one.
