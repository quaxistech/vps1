# AltMacro

AltMacro ingests external market and macro reference series, normalizes their
timestamps and units, records data quality and publishes the stored series to
PostgreSQL and JetStream. It preserves what a source reported; it does not
calculate correlations, forecasts, indicators or trading decisions.

Crypto market history remains owned by AltSpotter. AltMacro supplies external
reference data used by downstream analytics and forecasting services.

## Data contract

- Every instrument has a class: `MARKET`, `OFFICIAL_REFERENCE`,
  `ECONOMIC_RELEASE` or `DERIVED`.
- Source bindings define the provider symbol, price basis, timezone,
  timestamp meaning and priority. Fallbacks preserve the same price basis.
- Absence is explicit. Quality states are `VALID`, `STALE`, `MISSING`,
  `PROVIDER_ERROR`, `MARKET_CLOSED` and `PARTIAL`; a missing observation is
  never represented as zero.
- Market bars use event time. Reference values retain observation and
  publication times where available. Release observations retain observation
  time and vintage so consumers can select only information available at a
  given point in time.
- History before a source's actual coverage is left absent. The service does
  not synthesize values to fill a requested window.

See [docs/DATA_MODEL.md](docs/DATA_MODEL.md) for the schema and semantics.

## Providers

The binary implements Dukascopy, FRED, ALFRED and Frankfurter adapters. The
database registry defines instruments and provider bindings; the example
configuration controls which adapters are enabled. Tiingo and Twelve Data
appear as disabled registry entries but are not implemented by this binary.
Provider availability and active bindings can be inspected with
`altmacro inspect --what providers`.

Provider-specific source symbols and normalization rules are documented in
[docs/PROVIDERS.md](docs/PROVIDERS.md).

## Commands

```sh
altmacro run        --config PATH
altmacro bootstrap  --config PATH [--no-gap-scan]
altmacro inspect    --config PATH [--what registry|coverage|providers|all]
altmacro nats-setup --config PATH
altmacro selftest
```

`bootstrap` loads historical data and can be resumed. `inspect` reports the
registered instruments, stored coverage or provider bindings. `selftest`
checks offline invariants without a database or network connection.

The database password is supplied through `ALTMACRO_DB_PASSWORD`. Provider
credentials, when required by an enabled adapter, are read from their named
environment variables. Secrets do not belong in YAML.

## Build and deployment

The build presets and packaging scripts are in `CMakePresets.json` and
`scripts/`. Release creation runs the repository's configured checks; deployment
installs the packaged binary. Database bootstrap/migrations and JetStream
provisioning are separate operations.

## Documentation

Start at [docs/README.md](docs/README.md) for the data model, provider
contracts and routine operations.
