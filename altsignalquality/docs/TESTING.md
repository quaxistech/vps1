# Testing

The release preset configures the standard local suite:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

The domain suite covers feature assembly, labels, as-of priors, temporal splits,
calibration, metrics, policy and leakage rules. The I/O and wire suites cover
PostgreSQL codecs, deterministic identifiers and protobuf compatibility.
Contract scripts check vendored schemas and release contents.

An integration binary covers the durable consumer, database lookups,
evaluation writes, publication and redelivery with PostgreSQL and NATS
available. See the CMake targets and `tests/` for the configured integration
setup.

The `asan` preset runs the local suite with AddressSanitizer and
UndefinedBehaviorSanitizer:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
```
