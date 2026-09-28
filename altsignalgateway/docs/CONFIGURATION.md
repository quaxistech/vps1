# Configuration

The service uses `config/altsignalgateway.example.yaml`, an environment file
and a separate client credential file. A typical installation places them at:

```text
/etc/altsignalgateway/altsignalgateway.yaml
/etc/altsignalgateway/common.env
/etc/altsignalgateway/clients.yaml
```

Environment substitutions use `${VAR}`. Unset variables are configuration
errors. Keep database, NATS and client secret values out of the service YAML.
The credential format is shown in `config/clients.example.yaml`.

## Service configuration

| section | purpose |
|---|---|
| root | environment, logging, instance identity, PostgreSQL and NATS URLs |
| `wiring` | AltSignalQuality input stream/durable, source schemas, execution venue |
| `journal` | connection pool, delivery batches, expiry sweep and retention |
| `ingest` | JetStream fetch size and optional maximum signal age |
| `tls` | certificate, private key, TLS version and optional client certificate |
| `auth` | client credential file, signed-clock window and session lockout |
| `websocket` | listener, connection/queue bounds, heartbeat, resume and ACK timeouts |
| `query` | protocol-2 history/context query bounds |

The input stream belongs to AltSignalQuality. The Gateway creates its own
durable consumer and validates the producer stream. The journal and all client
cursors are stored in the Gateway-owned PostgreSQL schema.

## Client identities

Each client entry defines:

| field | meaning |
|---|---|
| `client_id` | logical client identity |
| `key_id` | credential selected during the authentication handshake |
| `secret_hex` | HMAC secret, supplied by environment substitution |
| `capabilities` | explicit grants: `receive`, `ack`, `resume`, `query` |
| `max_sessions` | maximum simultaneous sessions for the identity |

An observer can have `receive`, `resume` and `query` without `ack`. A client
that owns durable processing grants `ack` and coordinates acknowledgement of
its own `signal_id` idempotency keys. Client names and capabilities are
deployment choices; `clients.example.yaml` is a template, not a list of active
connections.

For credential rotation, add a second key identifier for the client, switch
the client to it, then remove the old entry. Protect the credentials file with
permissions that allow only the service operator and account to read it.

## Defaults and contract details

The example uses UTC, TLS 1.3, listener port 9443, 400-day journal retention,
and optional protocol-2 queries. Exact field defaults and bounds are declared
in `config/altsignalgateway.example.yaml` and validated at load time. See
[WEBSOCKET_PROTOCOL.md](WEBSOCKET_PROTOCOL.md) for protocol-level limits and
[SECURITY.md](SECURITY.md) for the trust boundary.

Validate a configured installation with:

```sh
altsignalgateway validate --config /etc/altsignalgateway/altsignalgateway.yaml
```
