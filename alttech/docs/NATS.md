# NATS

## Streams and durables

| resource | owner | AltTech's relationship to it |
|---|---|---|
| `ALTSPOTTER_BARS` | altspotter | read-only; AltTech creates only its own durable on it |
| `ALTSPOTTER_MARKET_CONTEXT`, `ALTSPOTTER_CONTROL` | altspotter | read-only |
| `ALTTECH_ANALYTICS` | **AltTech** | created and consumed by AltTech; subject filter `alttech.>` |
| `alttech-bars` (durable) | **AltTech** | AltTech's own pull consumer on `ALTSPOTTER_BARS` |

AltTech never attaches to an altspotter durable. Doing so would consume messages
altspotter's own writer needs, so the topology checker verifies ownership rather
than trusting configuration.

## Topology management

```sh
alttech-nats-setup --check              # read-only validation; start-up runs this
alttech-nats-setup --apply              # create ALTTECH_ANALYTICS and the durable
alttech-nats-setup --recreate-consumer  # replace AltTech's own durable
```

`--apply` is **create-only**: it never updates and never deletes. If a resource
exists with incompatible settings the tool reports it and exits non-zero rather
than reshaping something that may belong to someone else. Neither
`alttech.service` nor `alttech.target` ever runs apply mode — start-up performs
the read-only check and fails closed.

`--recreate-consumer` is the one exception, and it is deliberately narrow: it
deletes and recreates **AltTech's own durable** on the input stream, and touches
nothing else. It is the supported way to adopt a changed filter subject. The
durable's position is not history — PostgreSQL is — so recreating it costs
nothing the service cannot recompute.

Validation of an **externally owned** stream asks whether it *covers* the
subject AltTech reads, not whether the two strings are equal. altspotter
configures `ALTSPOTTER_BARS` as `bar.>` for its own needs, which covers the
`bar.bitget.>` filter AltTech uses; requiring equality would reject a perfectly
valid stream. AltTech's own stream is validated for an exact match, because
AltTech is the one that created it.

## Subjects AltTech reads

| subject | payload | why |
|---|---|---|
| `bar.bitget.>` (JetStream) | `Candle`, `CandleBatch` | the primary venue's closed bars, the feature module's live input. altspotter publishes all four venues under `bar.>`; the reference venues are read from PostgreSQL instead -- by the venue_lite sweep for the 31-value set and by the feature module's periodic catch-up for the 995-value set -- so taking delivery of them here would only build an unacknowledged backlog |
| `md.bitget.*.trade` (Core) | `Trade` | the primary venue's trade stream, for microstructure |
| `evt.universe.>` | universe events | react to a pair becoming eligible or ineligible |

## Subjects AltTech publishes

All under the owned `alttech.` namespace, so one stream captures everything.

**Per venue**

```
alttech.feature.venue.<venue>.<symbol>.<tf>.core.v1        VenueFeatureVector (primary)
alttech.feature.venue.<venue>.<symbol>.<tf>.reference.v1   VenueFeatureVector (reference)
alttech.microfeature.venue.<venue>.<symbol>.<window>.v1    VenueMicrostructureVector
alttech.structure.venue.<venue>.<symbol>.<tf>.v1           VenueStructureZone
alttech.structure.venue.<venue>.<symbol>.<tf>.<event>      StructureEvent
alttech.regime.venue.<venue>.<symbol>.<tf>.v1              RegimeSnapshot
alttech.regime.venue.<venue>.<symbol>.combined.v1          CombinedRegime
```

**Cross venue**

```
alttech.feature.cross_venue.<symbol>.<tf>.core.v1          CrossVenueFeatureVector
alttech.quality.cross_venue.<symbol>.<tf>.v1               CrossVenueQuality
alttech.regime.cross_venue.<symbol>.<tf>.v1                GlobalMarketRegime
alttech.structure.cross_venue.<symbol>.<tf>.v1             ConsensusStructureZone
alttech.structure.bitget_projected.<symbol>.<tf>.v1        BitgetProjectedZone
alttech.evt.cross_venue.<symbol>.{ready,degraded,invalid}  window lifecycle
```

**Status**

```
alttech.status.<module>.<venue>.<symbol>.<tf>.<kind>       StatusEvent
```

The profile segment (`core` / `reference`) lets a consumer subscribe to exactly
one profile. `<event>` is `created`, `tested`, `broken`, `flipped` or `expired`.

`.v1` is the public contract version, matching the `alttech.v1` protobuf
package. It changes only on a breaking change to the payload — never on a
release.

## Delivery guarantees

**Database first, then publish.** Every analytic is committed to PostgreSQL
before its event is published. PostgreSQL is the source of truth; the event is a
notification. A consumer that misses an event has lost nothing it cannot read
from the database.

**Deterministic message ids.** Every published message carries an id derived
from the natural key of the analytic — pair, venue, timeframe, bar. JetStream
deduplicates within its window, so a recomputed and republished bar does not
appear twice.

**Acknowledge after durability.** On the input side the feature module
acknowledges a bar only after its vector is committed. A crash between compute
and commit re-delivers the bar rather than losing it.

**PostgreSQL is the ordering barrier.** A bar can arrive on JetStream before
altspotter's own writer has committed it. The feature module NAKs until the
exact bar and revision are visible in the database, which keeps AltTech from
computing over a bar the system of record has not accepted yet.

## Consuming AltTech's output

Create your own uniquely named durable:

```
stream:  ALTTECH_ANALYTICS
durable: <yourproject>-<purpose>
filter:  alttech.feature.cross_venue.>
ack:     explicit — acknowledge after you have processed the message
```

Do not attach to `alttech-bars`: that is AltTech's own durable on altspotter's
stream, and attaching to it would take AltTech's input away.

## Credentials

Give AltTech its own NATS user rather than reusing altspotter's, scoped to what
it actually needs:

```
{ user: alttech_app, password: "…", permissions: {
    publish:   { allow: ["alttech.>", "$JS.API.>", "$JS.ACK.>"] },
    subscribe: { allow: ["bar.>", "md.>", "evt.universe.>", "alttech.>", "_INBOX.>"] } } }
```

Publishing is limited to the namespace AltTech owns, so a bug in AltTech cannot
write into altspotter's subjects. Applying this is a NATS config reload
(`systemctl reload nats-server`), which does not drop existing connections.
