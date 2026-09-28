# Testing

Build and run the registered test suite with the release preset:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

The test suite is organized into:

- `altstrategy_engine_tests` for context assembly, strategy evaluation, ranking,
  plan generation, proposal publication, outcomes, and attribution;
- `altstrategy_io_tests` for database/NATS-free decoding and snapshot helpers;
- shell integration checks for database and message-bus behavior, when the
  required local PostgreSQL and NATS tools are available.

Unit tests use deterministic fixtures. They do not connect to live exchanges or
a production database.
