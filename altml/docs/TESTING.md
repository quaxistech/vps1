# Testing

Configure and build with the release preset, then run the registered suite:

```sh
cmake --preset release
cmake --build --preset release
ctest --test-dir build/release --output-on-failure
```

The tests are organized into:

- **Unit tests** under `tests/unit/` for configuration, feature construction,
  label quality, training logic, inference logic, retention, and wire contracts.
- **Release contract** at `tests/shell/test_ml_release_contract.sh` for the
  installed runtime layout and service boundary.

The registered suite uses local fixtures and does not require live venue access.
