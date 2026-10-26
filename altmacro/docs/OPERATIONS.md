# Operations

## Service lifecycle

The `run` command starts the supervised polling and publishing service. It
requires the configured PostgreSQL database and, when enabled, NATS.

```sh
altmacro run --config /etc/altmacro/altmacro.yaml
systemctl status altmacro.service
systemctl restart altmacro.service
journalctl -u altmacro.service
```

## Bootstrap and inspection

Bootstrap retrieves the historical window from configuration. Set the required
`bootstrap.history_start` to the desired start within the available history;
there is no built-in date. Bootstrap is resumable, with progress tracked for
each instrument, provider, timeframe and price basis.

```sh
altmacro bootstrap --config /etc/altmacro/altmacro.yaml
altmacro bootstrap --config /etc/altmacro/altmacro.yaml --no-gap-scan
altmacro inspect --config /etc/altmacro/altmacro.yaml --what all
```

`inspect` accepts `registry`, `coverage`, `providers` or `all`. Review the
provider bindings and the source's reported coverage before using a series.

## Initialization

Apply the database migrations using the repository's migration scripts. Create
or update the JetStream stream with:

```sh
altmacro nats-setup --config /etc/altmacro/altmacro.yaml
```

The example configuration is `config/altmacro.example.yaml`. Supply the
database password using `ALTMACRO_DB_PASSWORD`; keep NATS credentials and any
provider tokens in the service environment file.

## Offline invariants

```sh
altmacro selftest
```

This command checks local invariants without connecting to PostgreSQL or a
provider. Release build and deployment scripts are under `scripts/`.

## HTTPS and correction publication

HTTPS verifies both the trusted certificate chain and the requested hostname
or IP SAN. There is no insecure fallback. Connect/TLS and request write/read
have explicit deadlines; shutdown cancels outstanding operations. DNS uses one
owned `/usr/bin/getent` child, preserving system NSS rules and allowing a stalled
lookup to be killed and reaped. Stock Ubuntu's `libc-bin` supplies this utility.
Request targets are excluded from transport error messages.

Publication IDs use semantic content, excluding transport `ingest_time`.
Identical retries retain the ID; value, quality or provenance corrections get
a different ID. Subjects, JSON schema and stream deduplication policy are
unchanged. The first rollout from the old natural-key ID can append one
equivalent transport message for a repolled observation; it is not a new source
fact. Consumers retain natural-key idempotency. AltTech's existing FDW reads
continue to use normalized database rows. Never purge streams for this change.
