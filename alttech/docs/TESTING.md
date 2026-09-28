# Testing

Run the configured release suite with:

```sh
ctest --preset release --output-on-failure
```

The local suite covers feature scheduling and warm-up semantics, feature
calculation, market-structure lifecycle, maintenance policy, schema checksums,
cross-venue estimators and aggregation, reference analytics, serialization,
JetStream topology and command-line validation.

The unit suite uses deterministic inputs and does not require PostgreSQL, NATS
or network access. End-to-end service behaviour depends on the configured
market-data, database and broker services.
