# Providers and source bindings

Provider adapters fetch source data and normalize it to the series contract.
The database registry is authoritative for instruments, native symbols, price
bases, timezones, timestamp semantics, decode parameters and fallback priority.
The YAML `instruments` list selects which registered series an instance
carries; it does not redefine their semantics.

## Implemented adapters

| adapter | data | credential |
|---|---|---|
| Dukascopy | Historical intraday market bars; bid and ask are separate series | None |
| FRED | Daily reference series | None for the configured graph endpoint |
| ALFRED | FRED series by vintage date | None for the configured graph endpoint |
| Frankfurter | Official/reference exchange rates from an explicitly selected source | None |

Tiingo and Twelve Data have disabled registry entries but no adapter in this
binary. A registry row alone does not mean an integration is available.

## Binding semantics

Each row in `macro_instrument_sources` binds one instrument to a provider and
native symbol. It also specifies:

- the published price basis and timestamp semantics;
- the source timezone and provider priority;
- optional decoding scale and sanity bounds for encoded prices;
- whether that source binding is enabled.

Fallbacks use the same instrument and price basis. A binding without required
source metadata is not treated as a usable provider. Use the inspection command
to review active and rejected bindings before bootstrapping.

## Normalization

Provider results are normalized to UTC and validated for positive prices,
coherent OHLC values, non-negative volume and instrument-specific sanity bounds.
Provider volume remains in the source's own unit and is not comparable across
providers. Missing observations and market closures are recorded using explicit
quality states, not fabricated prices.

## Rate limits and credentials

The registry records each source's access mode and rate-limit metadata. The
service applies its configured request pacing; inspect registry/provider
metadata for the values in the current build. Credential-bearing adapters read
their named environment variables at run time. Secret values are not stored in
the registry or YAML configuration.
