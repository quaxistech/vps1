# NATS and JetStream

AltSpotter publishes live market updates over Core NATS and retains selected events in three JetStream streams. Run <code>altspotter nats-init</code> to provision AltSpotter-owned streams and consumers. Consumer applications should create their own durable consumers; see the [Consumer guide](CONSUMER_GUIDE.md).

## Core NATS

Core NATS subjects carry live updates and have no retention:

    md.<venue>.<symbol>.trade
    md.<venue>.<symbol>.candle.1m.update
    md.<venue>.<symbol>.book.snapshot
    md.<venue>.<symbol>.book.delta

Raw trades and book deltas are live feeds. Subscribers receive messages published after they connect.

## JetStream streams

| Stream | Subject families | Retention |
|---|---|---|
| <code>ALTSPOTTER_BARS</code> | Closed candles, aggregates, and history batches | File storage, 30 days |
| <code>ALTSPOTTER_CONTROL</code> | Instrument, gateway, integrity, history, storage, and repair events | File storage, 30 days |
| <code>ALTSPOTTER_MARKET_CONTEXT</code> | BBO, ticker, mark, index, funding, open interest, and universe events | File storage, 14 days |

The owned durables are created by AltSpotter and are reserved for its internal writers and event handlers. Consumers must use a unique durable name and their own subject filter.

## Envelope

Every protobuf payload is carried in a versioned <code>Envelope</code>. It includes the venue, instrument identity, venue symbol, event timestamps, production timestamp, trace metadata, source channel, and quality state. The typed payload definitions are in <code>proto/</code>.

Use [Market-data semantics](MARKET_DATA_SEMANTICS.md) for timestamp, numeric, nullability, and provenance rules. The [Consumer guide](CONSUMER_GUIDE.md) describes subscription and decoding.
