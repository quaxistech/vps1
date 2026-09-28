# Testing

The release, debug, and sanitizer CMake presets enable the test suite. Configure and build the release preset with:

    cmake --preset release
    cmake --build --preset release

Run the configured suite with:

    ctest --test-dir build/release --output-on-failure

The test executable is <code>build/release/bin/altspotter_tests</code>. Catch2 tags can be used to run selected groups directly, for example:

    build/release/bin/altspotter_tests "[adapters],[book]"

## Test environments

Unit tests use local fixtures and do not require exchange access. The pipeline integration test starts the service against test NATS and PostgreSQL instances. It is skipped unless <code>ALTSPOTTER_TEST_NATS_SERVER</code> and <code>ALTSPOTTER_TEST_PG_DSN</code> are configured.

Tests do not validate live venue availability or the state of a deployed database. Deployment and service behavior are documented separately in [Deployment](DEPLOYMENT.md) and [Operations](OPERATIONS.md).
