# Testing

The release preset runs unit and contract tests:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

Unit tests cover signal validation and canonicalization, authentication,
protocol state, delivery transitions, client cursors, wire field numbers,
configuration and deterministic identifiers. Contract scripts check vendored
schemas and database transition definitions.

## Database invariants

`altsignalgateway selftest` checks journal, sequence, delivery-state,
immutability and cursor invariants in a database transaction that is rolled
back.

## Live suites

The shell suites in `tests/shell/` require a configured service stack and are
separate from CTest. They cover end-to-end signal delivery, restart/resume,
dependency and protocol failure scenarios, malformed input and load reporting.
The relevant suite setup is documented alongside those scripts.
