# AltSignalGateway

AltSignalGateway is the durable external delivery boundary for final
`TradeSignal` messages. It consumes AltSignalQuality output, validates and
enriches each signal, stores an immutable journal record and serves clients over
an authenticated TLS WebSocket.

```text
AltStrategy → AltSignalQuality → AltSignalGateway → WebSocket clients
```

The Gateway does not make trading decisions, alter a trade plan, train a model
or execute orders.

## Guarantees

- At-least-once delivery with persistent identity, sequence numbers,
  acknowledgements and resume support.
- Immutable journal identity and payload, enforced by database constraints.
- Payload hashes that clients can verify against the delivered bytes.
- Operation independent of a connected client; delivery state is stored in the
  journal.

See [docs/DELIVERY.md](docs/DELIVERY.md) for the delivery contract.

## Wire contract, ownership and recovery

The current boundary is protocol **2**, payload schema **3** (`vps1-signal-1`):
forecast block tag 11 and setup block tag 12 accompany the original proposal.
[Protobuf](proto/gateway.proto) defines exact fields; older journalled schema
rows retain their own version and presence semantics. Protocol 1 support is
compatibility, not the VPS2 ingest contract.

The `altsignalgateway` schema owns immutable canonical/source bytes and hashes,
sequence allocation, delivery/session history and per-client contiguous cursors.
Ingest ACKs JetStream only after journal commit. Delivery frames splice those
stored bytes verbatim; consumers hash the original payload-field bytes before
parsing, never a re-serialization. ACK means durable receipt, not a trade.

Restart resumes the durable journal/cursors with bounded in-memory credit.
Expired signals are no longer owed; skipped sequence ranges are explicit in
resume metadata. A `client_id` owns one cursor: production, staging and observer
identities must be separate. A superseded executor cannot safely reconnect in
an eviction loop. Read-only queries require their own capability/session and
never move a delivery cursor. The Gateway reads upstream T0 enrichment without
altering setup prices, recommendation or provenance; absent context stays absent.

See [configuration](docs/CONFIGURATION.md), [architecture](docs/ARCHITECTURE.md)
and [consumer guide](docs/CONSUMER_GUIDE.md). The diagnostic client verifies exact
received payload bytes in delivery and signal/context query frames, with bounds
and duplicate-field rejection. History pages carry summaries, not payloads.
The original audit baseline is retained in [source limitations](../docs/source-contract-limitations.md).

## Commands

```text
altsignalgateway run
altsignalgateway nats-setup
altsignalgateway validate
altsignalgateway status
altsignalgateway inspect
altsignalgateway verify
altsignalgateway selftest
altsignalgateway client
```

Run `altsignalgateway <command> --help` for command options.

## Build and deploy

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
./scripts/deploy.sh
```

Database provisioning, NATS setup, TLS material, client credentials and the
systemd service are managed separately from release installation. See
[docs/OPERATIONS.md](docs/OPERATIONS.md).

## Documentation

Start at [docs/README.md](docs/README.md). The
[consumer guide](docs/CONSUMER_GUIDE.md) covers client integration;
[SIGNAL_CONTRACT.md](docs/SIGNAL_CONTRACT.md) defines the payload;
[WEBSOCKET_PROTOCOL.md](docs/WEBSOCKET_PROTOCOL.md) defines frames, auth and
resume; [SECURITY.md](docs/SECURITY.md) describes the trust boundary.
