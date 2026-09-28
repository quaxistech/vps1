# Connecting to AltTech — data access for other projects

AltTech publishes analytics over the four USDT-perpetual venues
**Bitget** (`bitget`), **Binance USDⓈ-M** (`binance_usdm`), **Bybit linear**
(`bybit`) and **OKX USDT swap** (`okx`), in two ways:

- **PostgreSQL** — the durable analytics, in the `alttech` schema. Use this for
  backtests, feature sets, model training and post-trade attribution.
- **NATS** — the same analytics as low-latency events on subjects under
  `alttech.`, captured by the `ALTTECH_ANALYTICS` JetStream stream. Use this to
  react in real time.

AltTech never writes to altspotter's `public` schema and never touches an
exchange. Market data itself comes from altspotter; see its `connect.md`.

Both PostgreSQL and NATS bind to `127.0.0.1` only, so a consumer runs on the
same host or reaches it through an SSH tunnel.

---

## 1. Keys and identifiers

Every analytics row is keyed on `canonical_pair_id`, the pair id shared across
venues (altspotter's `canonical_pairs.id`, and its `instrument_id` in the market
tables). Per-venue rows add `exchange_id`: `1` bitget, `2` binance_usdm, `3`
okx, `4` bybit — the same numbering as the `Exchange` protobuf enum.

Venue bitmasks set bit `exchange_id - 1`: bit 0 Bitget, bit 1 Binance, bit 2
OKX, bit 3 Bybit.

All timestamps are `TIMESTAMPTZ` in UTC. Analytics values are `REAL`/`DOUBLE
PRECISION`; prices carried into the analytics stay `NUMERIC`.

---

## 2. PostgreSQL

### Connection

Schema `alttech` in AltTech's own database (separate from altspotter's).
Create a least-privilege reader:

```sql
CREATE ROLE alttech_reader LOGIN PASSWORD '<generate-a-strong-password>';
GRANT CONNECT ON DATABASE alttech TO alttech_reader;
GRANT USAGE ON SCHEMA alttech TO alttech_reader;
GRANT SELECT ON ALL TABLES IN SCHEMA alttech TO alttech_reader;
ALTER DEFAULT PRIVILEGES IN SCHEMA alttech GRANT SELECT ON TABLES TO alttech_reader;
```

### Tables you read

| table | what | key |
|---|---|---|
| `venue_feature_vectors` | ordered feature vector per (venue, pair, timeframe, bar) | `(venue_class, exchange_id, canonical_pair_id, timeframe, bar_open_time, bar_revision, feature_set_version_id)` |
| `venue_feature_projection` | flat columns (EMA/RSI/ATR/ADX/BB width/…) for cheap SQL joins | `(exchange_id, canonical_pair_id, timeframe, bar_open_time)` |
| `venue_microstructure_vectors` | trade/book features per window | `(part_class, exchange_id, canonical_pair_id, window_label, window_start, feature_set_version_id)` |
| `venue_structure_zones` | per-venue support/resistance zones with lifecycle | `id` |
| `structure_snapshots` | the active zone set at one closed bar | `(exchange_id, canonical_pair_id, timeframe, bar_open_time)` |
| `structure_zone_events` | zone lifecycle transitions | — |
| `venue_regimes` | per-venue regime per bar | `(exchange_id, canonical_pair_id, timeframe, bar_open_time)` |
| `regime_events` | regime transitions, venue and cross-venue | — |
| `cross_venue_feature_vectors` | the consensus feature vector | `(canonical_pair_id, timeframe, bar_open_time, feature_set_version_id)` |
| `cross_venue_snapshots` | compact consensus view (mid/mark/index, deviation, leader) | `(canonical_pair_id, timeframe, bar_open_time)` |
| `cross_venue_quality` | how trustworthy one window is, and why | `(canonical_pair_id, timeframe, bar_open_time)` |
| `cross_venue_components` | each venue's weight and inclusion in the consensus | `(canonical_pair_id, timeframe, bar_open_time, exchange_id)` |
| `venue_alignment_state` | event-time alignment of one bar window | `(canonical_pair_id, timeframe, canonical_open_time)` |
| `global_regimes` | the regime the four venues agree on | `(canonical_pair_id, timeframe, bar_open_time)` |
| `consensus_structure_zones` | zones the venues agree on, in normalized coordinates | `id` |
| `bitget_projected_zones` | those zones projected onto the primary venue's prices | `id` |
| `feature_set_versions`, `feature_set_members` | what each position in a `values` array means | — |

### Decoding a feature vector

`values` is a `REAL[]`; `validity_bitmap` is a `BYTEA` where bit `i` set means
`values[i]` is usable. A value whose bit is clear was not computable and must
not be read — it is not zero, it is absent. The meaning of position `i` comes
from the feature set:

```sql
SELECT m.position, m.output_name
FROM alttech.feature_set_members m
JOIN alttech.feature_set_versions v ON v.id = m.feature_set_version_id
WHERE v.name = 'talib-core' AND v.version = 1
ORDER BY m.position;
```

Feature sets are frozen: once a version exists, its positions never move.
Adding a metric creates a new version.

### Example reads

```sql
-- the last hour of primary-venue 1m features for one pair
SELECT bar_open_time, values, validity_bitmap, status
FROM alttech.venue_feature_vectors
WHERE venue_class = 'bitget' AND exchange_id = 1 AND canonical_pair_id = :pair
  AND timeframe = '1m' AND bar_open_time >= now() - interval '1 hour'
ORDER BY bar_open_time;

-- only windows where all four venues agreed
SELECT bar_open_time, consensus_mid_price, bitget_mid_deviation_bps
FROM alttech.cross_venue_snapshots
WHERE canonical_pair_id = :pair AND quality_status = 'STRICT_VALID'
  AND bar_open_time >= now() - interval '6 hours'
ORDER BY bar_open_time;

-- the current global regime and how unanimous it is
SELECT primary_regime, regime_confidence, venue_agreement, venue_dispersion,
       bitget_regime_divergence
FROM alttech.global_regimes
WHERE canonical_pair_id = :pair AND timeframe = '1m'
ORDER BY bar_open_time DESC LIMIT 1;

-- consensus zones projected onto tradable Bitget prices
SELECT c.kind, p.projected_lower, p.projected_center, p.projected_upper,
       c.supporting_venue_count, p.projection_quality
FROM alttech.consensus_structure_zones c
JOIN alttech.bitget_projected_zones p ON p.source_consensus_zone_id = c.id
WHERE c.canonical_pair_id = :pair AND c.lifecycle IN ('ACTIVE','TESTED')
ORDER BY p.projected_center;
```

**Always check quality before trusting a row.** `cross_venue_quality.quality_status`
is `STRICT_VALID` (all four venues), `DEGRADED_3_OF_4`, `DEGRADED_2_OF_4` or
`INVALID`; `venue_feature_vectors.status` is `VALID`, `WARMING` (the indicator
has not converged), `INCOMPLETE_INPUT` or `INVALID`.

**Retention is short by design.** AltTech is sized for a trading horizon of at
most 48 hours, so most families keep only a few days. See
[docs/STORAGE.md](docs/STORAGE.md) for the exact windows; do not assume anything
older than the documented retention still exists.

---

## 3. NATS

### Connection

`nats://127.0.0.1:4222`, authenticated. AltTech's outputs are captured by the
JetStream stream **`ALTTECH_ANALYTICS`** with the subject filter `alttech.>`.

### Subjects

| subject | payload |
|---|---|
| `alttech.feature.venue.<venue>.<symbol>.<tf>.core.v1` | `VenueFeatureVector` (primary venue, full set) |
| `alttech.feature.venue.<venue>.<symbol>.<tf>.reference.v1` | `VenueFeatureVector` (reference venue) |
| `alttech.microfeature.venue.<venue>.<symbol>.<window>.v1` | `VenueMicrostructureVector` |
| `alttech.structure.venue.<venue>.<symbol>.<tf>.v1` | `VenueStructureZone` |
| `alttech.structure.venue.<venue>.<symbol>.<tf>.{created,tested,broken,flipped,expired}` | `StructureEvent` |
| `alttech.regime.venue.<venue>.<symbol>.<tf>.v1` | `RegimeSnapshot` |
| `alttech.regime.venue.<venue>.<symbol>.combined.v1` | `CombinedRegime` |
| `alttech.feature.cross_venue.<symbol>.<tf>.core.v1` | `CrossVenueFeatureVector` |
| `alttech.quality.cross_venue.<symbol>.<tf>.v1` | `CrossVenueQuality` |
| `alttech.regime.cross_venue.<symbol>.<tf>.v1` | `GlobalMarketRegime` |
| `alttech.structure.cross_venue.<symbol>.<tf>.v1` | `ConsensusStructureZone` |
| `alttech.structure.bitget_projected.<symbol>.<tf>.v1` | `BitgetProjectedZone` |
| `alttech.evt.cross_venue.<symbol>.{ready,degraded,invalid}` | window lifecycle |
| `alttech.status.<module>.<venue>.<symbol>.<tf>.<kind>` | `StatusEvent` |

`.v1` is the contract version and changes only on a breaking change.

### Consuming

Create your **own uniquely named** durable pull consumer — do not attach to
AltTech's `alttech-bars` durable on altspotter's stream, you would steal its
messages.

```
stream:  ALTTECH_ANALYTICS
durable: myproject-crossvenue
filter:  alttech.feature.cross_venue.>
ack:     explicit (ack AFTER you have processed the message)
```

Every event is committed to PostgreSQL before it is published, so the database
is the source of truth and an event you miss is never lost — replay it from
PostgreSQL. Message ids are deterministic, so JetStream deduplicates a window
that was recomputed and re-published.

### Decoding

Every payload is a serialized `Envelope` wrapping one typed message. Compile
the `proto/` directory of this repository — it is the contract, keep it
byte-identical. Then:

1. parse the bytes into `Envelope`;
2. check `schema_version`;
3. read the `payload` oneof — the field that is set names the message.

Decimal money fields use the `Decimal` message (mantissa and scale); never
reconstruct them through `double`.

---

## 4. Which channel for which job

| you want… | use |
|---|---|
| react to a closed bar's features immediately | NATS `alttech.feature.venue.bitget.*` |
| react to the four venues disagreeing | NATS `alttech.quality.cross_venue.*` / `alttech.evt.cross_venue.*` |
| levels to trade against | `bitget_projected_zones` joined to `consensus_structure_zones` |
| a regime filter for a strategy | `global_regimes`, or `venue_regimes` for one venue |
| training data / backtests | PostgreSQL `alttech` tables, within the retention window |
| replay after downtime | PostgreSQL; JetStream only covers the stream's own age |

## 5. Checklist

- Compile this repository's **`proto/`** unchanged.
- Read the **validity bitmap**: an invalid position is absent, not zero.
- Read the **quality status** before acting on a cross-venue value.
- Use a **read-only role** and your **own durable name**.
- Everything is **UTC**; never round-trip money through `double`.
- Respect the **retention window** — old analytics are dropped, not archived.
