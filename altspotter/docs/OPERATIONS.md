# Operations

This page lists routine service commands and the state they report.

## Commands

| Command | Purpose |
|---|---|
| <code>altspotter version</code> | Build and version metadata |
| <code>altspotter config-check</code> | Configuration validation |
| <code>altspotter inspect --universe</code> | Instrument eligibility and membership |
| <code>altspotter inspect --exchanges</code> | Venue connection state and active instruments |
| <code>altspotter inspect --quality</code> | Per-venue data-quality state |
| <code>altspotter inspect --coverage</code> | One-minute candle coverage |
| <code>altspotter inspect --retention</code> | Retention policies and recent runs |
| <code>altspotter inspect --storage</code> | Database budget state and table sizes |
| <code>altspotter inspect --growth</code> | Table growth measurements |
| <code>altspotter retention-run --dry-run</code> | Preview a one-shot retention pass |

The service lifecycle is managed by systemd:

    systemctl start altspotter.service
    systemctl stop altspotter.service
    systemctl status altspotter.service

## Readiness

The systemd unit uses <code>Type=notify</code>. Process readiness covers the shared PostgreSQL and NATS dependencies and the storage writer. Venue state is reported independently, so a reconnecting venue does not prevent unaffected venues from continuing to operate.

Per-venue connection state is stored in <code>exchange_connections</code> and included in the exchange inspection report. Data-quality state and coverage are available through their respective inspection modes and database tables.

## Retention

Retention runs automatically inside <code>altspotter run</code>. A one-shot dry run is available through <code>altspotter retention-run --dry-run</code>. Policy details are documented in [Retention](RETENTION.md).
