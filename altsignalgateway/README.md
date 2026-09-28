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
