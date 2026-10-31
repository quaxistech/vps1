# Testing

Build and run the registered test suite with the release preset:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

(`scripts/build-release.sh` configures, builds and runs this same `release`
preset with tests enabled; any failing test stops packaging. An integration
case that Catch2 skips for a missing fixture exits 4 and CTest records it as
skipped, not passed, so set `PGBIN` when packaging a release.)

The test suite is organized into:

- `altstrategy_engine_tests` for context assembly, strategy evaluation, ranking,
  plan generation (units, sides, the tick grid: `test_engine_plan_units.cpp`),
  proposal publication, outcomes (evaluator 2: `test_engine_outcomes_v2.cpp`)
  and attribution;
- `altstrategy_io_tests` for database/NATS-free decoding and snapshot helpers;
- `altstrategy_integration_tests` (label `integration`) against a real,
  ephemeral PostgreSQL 16 cluster with every migration applied and a real
  `nats-server` with JetStream: the outbox (archive and publish in one commit,
  every published message's archive references, rollback, stream and database
  outages, a lost ack, restart, a re-decided context sending nothing twice,
  dead-letter kept through retention and sent once requeued), a ranking that
  cannot be archived, the restart of the hypothesis book, live maturation
  through evaluator 2, and the schema version the service requires. It needs
  `PGBIN` (the directory holding `initdb`, `pg_ctl`, `psql`) and `nats-server`
  on `PATH`, runs as a non-root user, and skips without them:

  ```sh
  PGBIN=/usr/lib/postgresql/16/bin ./build/release/bin/altstrategy_integration_tests
  ```

- `sql_insert_arity` (`tests/check_sql_arity.py`): every parameterised INSERT
  names as many columns as it binds values.
- `tests/shell/test_db_failure.sh` (not in ctest): the real service, migrated by
  the real `altstrategy-db-migrate`, through a `kill -9` of PostgreSQL, the
  singleton lock and SIGTERM, under systemd user units.

The `asan` preset (Debug, ASan + UBSan) runs the same suites. The vendored
nats.c v3.13.0 passes NULL with a zero length to `memcpy` (UBSan
`nonnull-attribute`, `glib_dispatch_pool.c`, `msg.c`); suppress exactly that
for its sources and nothing else:

```sh
printf 'nonnull-attribute:*/natsc-src/*\n' > /tmp/ubsan.supp
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=/tmp/ubsan.supp \
  PGBIN=... ./build/asan/bin/altstrategy_integration_tests
```

Unit tests use deterministic fixtures. They do not connect to live exchanges or
a production database.
