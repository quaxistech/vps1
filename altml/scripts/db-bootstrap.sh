#!/usr/bin/env bash
# One-time, idempotent database bootstrap for AltML.
#
# Creates AltML's own database and role, a read-only reader role in each
# upstream service's database (AltSpotter, AltTech), wires postgres_fdw
# foreign tables for the historical data AltML reads read-only, and applies
# db/schema.sql. All three databases live in the same local PostgreSQL
# cluster, so admin operations use the local Unix-socket peer connection
# (run as: `sudo -u postgres scripts/db-bootstrap.sh`); only the FDW SERVER
# objects need a real TCP host/port, since postgres_fdw always connects that
# way regardless of how this script itself connects.
#
# All connection parameters come from the environment (never hardcoded here):
#   ALTML_DB_NAME / ALTML_DB_USER / ALTML_DB_PASSWORD   (AltML's own database)
#   ALTSPOTTER_FDW_HOST/PORT/DBNAME/USER/PASSWORD        (read-only reader)
#   ALTTECH_FDW_HOST/PORT/DBNAME/USER/PASSWORD           (read-only reader)
set -euo pipefail

: "${ALTML_DB_NAME:?set ALTML_DB_NAME}"
: "${ALTML_DB_USER:?set ALTML_DB_USER}"
: "${ALTML_DB_PASSWORD:?set ALTML_DB_PASSWORD}"
: "${ALTSPOTTER_FDW_HOST:?set ALTSPOTTER_FDW_HOST}"
: "${ALTSPOTTER_FDW_PORT:?set ALTSPOTTER_FDW_PORT}"
: "${ALTSPOTTER_FDW_DBNAME:?set ALTSPOTTER_FDW_DBNAME}"
: "${ALTSPOTTER_FDW_USER:?set ALTSPOTTER_FDW_USER}"
: "${ALTSPOTTER_FDW_PASSWORD:?set ALTSPOTTER_FDW_PASSWORD}"
: "${ALTTECH_FDW_HOST:?set ALTTECH_FDW_HOST}"
: "${ALTTECH_FDW_PORT:?set ALTTECH_FDW_PORT}"
: "${ALTTECH_FDW_DBNAME:?set ALTTECH_FDW_DBNAME}"
: "${ALTTECH_FDW_USER:?set ALTTECH_FDW_USER}"
: "${ALTTECH_FDW_PASSWORD:?set ALTTECH_FDW_PASSWORD}"

PGHOST="${PGHOST:-127.0.0.1}"
PGPORT="${PGPORT:-5432}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCHEMA_FILE="${1:-$SCRIPT_DIR/../db/schema.sql}"

# Local Unix-socket admin connection (peer auth as the `postgres` OS user).
psql_admin() { psql -v ON_ERROR_STOP=1 -U postgres "$@"; }

echo "==> AltML role and database"
psql_admin -d postgres -tAc "SELECT 1 FROM pg_roles WHERE rolname = '${ALTML_DB_USER}'" | grep -q 1 || \
    psql_admin -d postgres -c "CREATE ROLE ${ALTML_DB_USER} LOGIN PASSWORD '${ALTML_DB_PASSWORD}'"
psql_admin -d postgres -c "ALTER ROLE ${ALTML_DB_USER} PASSWORD '${ALTML_DB_PASSWORD}'"
psql_admin -d postgres -tAc "SELECT 1 FROM pg_database WHERE datname = '${ALTML_DB_NAME}'" | grep -q 1 || \
    psql_admin -d postgres -c "CREATE DATABASE ${ALTML_DB_NAME} OWNER ${ALTML_DB_USER}"

echo "==> Read-only reader role in AltSpotter's database"
psql_admin -d "$ALTSPOTTER_FDW_DBNAME" \
    -tAc "SELECT 1 FROM pg_roles WHERE rolname = '${ALTSPOTTER_FDW_USER}'" | grep -q 1 || \
    psql_admin -d "$ALTSPOTTER_FDW_DBNAME" \
        -c "CREATE ROLE ${ALTSPOTTER_FDW_USER} LOGIN PASSWORD '${ALTSPOTTER_FDW_PASSWORD}'"
psql_admin -d "$ALTSPOTTER_FDW_DBNAME" -c "
    GRANT CONNECT ON DATABASE ${ALTSPOTTER_FDW_DBNAME} TO ${ALTSPOTTER_FDW_USER};
    GRANT USAGE ON SCHEMA public TO ${ALTSPOTTER_FDW_USER};
    GRANT SELECT ON market_candles_1m, exchange_instruments, exchanges TO ${ALTSPOTTER_FDW_USER};
"

echo "==> Read-only reader role in AltTech's database"
psql_admin -d "$ALTTECH_FDW_DBNAME" \
    -tAc "SELECT 1 FROM pg_roles WHERE rolname = '${ALTTECH_FDW_USER}'" | grep -q 1 || \
    psql_admin -d "$ALTTECH_FDW_DBNAME" \
        -c "CREATE ROLE ${ALTTECH_FDW_USER} LOGIN PASSWORD '${ALTTECH_FDW_PASSWORD}'"
psql_admin -d "$ALTTECH_FDW_DBNAME" -c "
    GRANT CONNECT ON DATABASE ${ALTTECH_FDW_DBNAME} TO ${ALTTECH_FDW_USER};
    GRANT USAGE ON SCHEMA alttech TO ${ALTTECH_FDW_USER};
    GRANT SELECT ON alttech.feature_set_versions, alttech.feature_set_members,
        alttech.venue_feature_vectors TO ${ALTTECH_FDW_USER};
"

echo "==> AltML schema, extensions, foreign servers"
# CREATE EXTENSION/SERVER/FOREIGN TABLE all require superuser (postgres_fdw is
# not a trusted extension), so this whole block runs as the postgres OS user
# over the local socket; the schema itself is owned by altml_app so its own
# later `psql -f db/schema.sql` run can create ordinary tables inside it.
psql_admin -d "$ALTML_DB_NAME" <<SQL
CREATE EXTENSION IF NOT EXISTS postgres_fdw;
CREATE SCHEMA IF NOT EXISTS altml AUTHORIZATION ${ALTML_DB_USER};

-- use_remote_estimate is NOT optional here. Without it postgres_fdw plans a
-- correlated join with local heuristics, never produces a parameterized foreign
-- scan, and pulls the whole remote table to filter it locally: a single-row
-- candle lookup measured 5.2 s with "Rows Removed by Join Filter: 950169", and
-- 2.4 ms with remote estimates on. Every query here joins a local table to a
-- remote candle, so this option decides whether the service works at all.
-- fetch_size raises the cursor batch from the 100-row default for the bulk
-- history reads.
DO \$do\$ BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_foreign_server WHERE srvname = 'altspotter_srv') THEN
        CREATE SERVER altspotter_srv FOREIGN DATA WRAPPER postgres_fdw
            OPTIONS (host '${ALTSPOTTER_FDW_HOST}', port '${ALTSPOTTER_FDW_PORT}',
                     dbname '${ALTSPOTTER_FDW_DBNAME}',
                     use_remote_estimate 'true', fetch_size '10000');
    END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_foreign_server WHERE srvname = 'alttech_srv') THEN
        CREATE SERVER alttech_srv FOREIGN DATA WRAPPER postgres_fdw
            OPTIONS (host '${ALTTECH_FDW_HOST}', port '${ALTTECH_FDW_PORT}',
                     dbname '${ALTTECH_FDW_DBNAME}',
                     use_remote_estimate 'true', fetch_size '10000');
    END IF;
END \$do\$;

DROP USER MAPPING IF EXISTS FOR ${ALTML_DB_USER} SERVER altspotter_srv;
CREATE USER MAPPING FOR ${ALTML_DB_USER} SERVER altspotter_srv
    OPTIONS (user '${ALTSPOTTER_FDW_USER}', password '${ALTSPOTTER_FDW_PASSWORD}');
DROP USER MAPPING IF EXISTS FOR ${ALTML_DB_USER} SERVER alttech_srv;
CREATE USER MAPPING FOR ${ALTML_DB_USER} SERVER alttech_srv
    OPTIONS (user '${ALTTECH_FDW_USER}', password '${ALTTECH_FDW_PASSWORD}');

DROP FOREIGN TABLE IF EXISTS altml.fdw_candles_1m;
CREATE FOREIGN TABLE altml.fdw_candles_1m (
    exchange_id     SMALLINT    NOT NULL,
    instrument_id   BIGINT      NOT NULL,
    open_time       TIMESTAMPTZ NOT NULL,
    open            NUMERIC     NOT NULL,
    high            NUMERIC     NOT NULL,
    low             NUMERIC     NOT NULL,
    close           NUMERIC     NOT NULL,
    is_closed       BOOLEAN     NOT NULL,
    quality_status  TEXT        NOT NULL,
    revision        BIGINT      NOT NULL,
    updated_at      TIMESTAMPTZ NOT NULL
) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'market_candles_1m');

DROP FOREIGN TABLE IF EXISTS altml.fdw_exchange_instruments;
CREATE FOREIGN TABLE altml.fdw_exchange_instruments (
    exchange_id     SMALLINT NOT NULL,
    instrument_id   BIGINT   NOT NULL,
    exchange_symbol TEXT     NOT NULL,
    status          TEXT     NOT NULL,
    is_whitelisted  BOOLEAN  NOT NULL
) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'exchange_instruments');

DROP FOREIGN TABLE IF EXISTS altml.fdw_exchanges;
CREATE FOREIGN TABLE altml.fdw_exchanges (
    id   SMALLINT NOT NULL,
    code TEXT     NOT NULL
) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'exchanges');

DROP FOREIGN TABLE IF EXISTS altml.fdw_feature_set_versions;
CREATE FOREIGN TABLE altml.fdw_feature_set_versions (
    id             BIGINT  NOT NULL,
    name           TEXT    NOT NULL,
    version        INTEGER NOT NULL,
    engine_version TEXT    NOT NULL,
    schema_hash    TEXT    NOT NULL,
    frozen         BOOLEAN NOT NULL
) SERVER alttech_srv OPTIONS (schema_name 'alttech', table_name 'feature_set_versions');

DROP FOREIGN TABLE IF EXISTS altml.fdw_feature_set_members;
CREATE FOREIGN TABLE altml.fdw_feature_set_members (
    feature_set_version_id BIGINT  NOT NULL,
    position               INTEGER NOT NULL,
    output_name            TEXT    NOT NULL
) SERVER alttech_srv OPTIONS (schema_name 'alttech', table_name 'feature_set_members');

-- The upstream feature vectors themselves. AltTech retains only days of these
-- (its table is sized for live order attribution, not model training), so AltML
-- copies what it needs into its own archive before that window closes. Reading
-- them over the wrapper is what makes the archive independent of how long the
-- NATS stream happens to hold a message: the stream is capped by bytes and in
-- practice carries only hours.
DROP FOREIGN TABLE IF EXISTS altml.fdw_venue_feature_vectors;
CREATE FOREIGN TABLE altml.fdw_venue_feature_vectors (
    exchange_id            SMALLINT    NOT NULL,
    canonical_pair_id      BIGINT      NOT NULL,
    timeframe              TEXT        NOT NULL,
    bar_open_time          TIMESTAMPTZ NOT NULL,
    bar_revision           BIGINT      NOT NULL,
    feature_set_version_id BIGINT      NOT NULL,
    values                 REAL[]      NOT NULL,
    validity_bitmap        BYTEA       NOT NULL,
    status                 TEXT        NOT NULL,
    engine_version         TEXT        NOT NULL,
    computed_at            TIMESTAMPTZ NOT NULL
) SERVER alttech_srv OPTIONS (schema_name 'alttech', table_name 'venue_feature_vectors');

-- Cross-venue consensus vectors. Not reachable any other way for history: the
-- NATS stream is capped by bytes and carries hours, while AltTech's own table
-- carries days.
DROP FOREIGN TABLE IF EXISTS altml.fdw_cross_venue_feature_vectors;
CREATE FOREIGN TABLE altml.fdw_cross_venue_feature_vectors (
    canonical_pair_id      BIGINT      NOT NULL,
    timeframe              TEXT        NOT NULL,
    bar_open_time          TIMESTAMPTZ NOT NULL,
    feature_set_version_id BIGINT      NOT NULL,
    method_version         TEXT        NOT NULL,
    values                 REAL[]      NOT NULL,
    validity_bitmap        BYTEA       NOT NULL,
    status                 TEXT        NOT NULL,
    quality_status         TEXT        NOT NULL,
    computed_at            TIMESTAMPTZ NOT NULL
) SERVER alttech_srv OPTIONS (schema_name 'alttech', table_name 'cross_venue_feature_vectors');

-- Microstructure windows. The 1m window is the only one the bar-keyed archive
-- can hold; reference_lite_micro for the information venues is published on NO
-- NATS subject at all, so this wrapper is the only path to it.
DROP FOREIGN TABLE IF EXISTS altml.fdw_venue_microstructure_vectors;
CREATE FOREIGN TABLE altml.fdw_venue_microstructure_vectors (
    exchange_id            SMALLINT    NOT NULL,
    canonical_pair_id      BIGINT      NOT NULL,
    window_label           TEXT        NOT NULL,
    window_start           TIMESTAMPTZ NOT NULL,
    feature_set_version_id BIGINT      NOT NULL,
    values                 REAL[]      NOT NULL,
    validity_bitmap        BYTEA       NOT NULL,
    status                 TEXT        NOT NULL,
    engine_version         TEXT        NOT NULL,
    computed_at            TIMESTAMPTZ NOT NULL
) SERVER alttech_srv OPTIONS (schema_name 'alttech', table_name 'venue_microstructure_vectors');

-- The macro / cross-asset block. Scope GLOBAL: it belongs to no venue and no
-- pair, so it carries no instrument key at all and is joined to every target.
-- Published on no NATS subject, like reference_lite_micro, so this wrapper is
-- the only path to it.
DROP FOREIGN TABLE IF EXISTS altml.fdw_macro_feature_vectors;
CREATE FOREIGN TABLE altml.fdw_macro_feature_vectors (
    timeframe              TEXT        NOT NULL,
    bar_open_time          TIMESTAMPTZ NOT NULL,
    feature_set_version_id BIGINT      NOT NULL,
    method_version         TEXT        NOT NULL,
    values                 REAL[]      NOT NULL,
    validity_bitmap        BYTEA       NOT NULL,
    status                 TEXT        NOT NULL,
    valid_count            INTEGER     NOT NULL,
    computed_at            TIMESTAMPTZ NOT NULL
) SERVER alttech_srv OPTIONS (schema_name 'alttech', table_name 'macro_feature_vectors');

GRANT USAGE ON FOREIGN SERVER altspotter_srv TO ${ALTML_DB_USER};
GRANT USAGE ON FOREIGN SERVER alttech_srv TO ${ALTML_DB_USER};
GRANT SELECT ON altml.fdw_candles_1m, altml.fdw_exchange_instruments, altml.fdw_exchanges,
    altml.fdw_feature_set_versions, altml.fdw_feature_set_members,
    altml.fdw_venue_feature_vectors, altml.fdw_cross_venue_feature_vectors,
    altml.fdw_venue_microstructure_vectors, altml.fdw_macro_feature_vectors
    TO ${ALTML_DB_USER};

-- ANALYZE on a foreign table runs under its OWNER's user mapping, not the
-- caller's; only altml_app has a user mapping on these servers, so ownership
-- must move there before this database's own ANALYZE (below) can succeed.
ALTER FOREIGN TABLE altml.fdw_candles_1m OWNER TO ${ALTML_DB_USER};
ALTER FOREIGN TABLE altml.fdw_exchange_instruments OWNER TO ${ALTML_DB_USER};
ALTER FOREIGN TABLE altml.fdw_exchanges OWNER TO ${ALTML_DB_USER};
ALTER FOREIGN TABLE altml.fdw_feature_set_versions OWNER TO ${ALTML_DB_USER};
ALTER FOREIGN TABLE altml.fdw_feature_set_members OWNER TO ${ALTML_DB_USER};
ALTER FOREIGN TABLE altml.fdw_venue_feature_vectors OWNER TO ${ALTML_DB_USER};
ALTER FOREIGN TABLE altml.fdw_cross_venue_feature_vectors OWNER TO ${ALTML_DB_USER};
ALTER FOREIGN TABLE altml.fdw_venue_microstructure_vectors OWNER TO ${ALTML_DB_USER};
ALTER FOREIGN TABLE altml.fdw_macro_feature_vectors OWNER TO ${ALTML_DB_USER};
SQL
echo "==> Applying db/schema.sql"
PGPASSWORD="$ALTML_DB_PASSWORD" psql -v ON_ERROR_STOP=1 -h 127.0.0.1 -p 5432 \
    -U "$ALTML_DB_USER" -d "$ALTML_DB_NAME" -f "$SCHEMA_FILE"

CHECKSUM="$(sha256sum "$SCHEMA_FILE" | cut -d' ' -f1)"
PGPASSWORD="$ALTML_DB_PASSWORD" psql -v ON_ERROR_STOP=1 -h 127.0.0.1 -p 5432 \
    -U "$ALTML_DB_USER" -d "$ALTML_DB_NAME" -c "
    INSERT INTO altml.schema_version (singleton, checksum) VALUES (true, '${CHECKSUM}')
    ON CONFLICT (singleton) DO UPDATE SET checksum = EXCLUDED.checksum, applied_at = now();
"

echo "==> Collecting statistics on foreign tables (without this the planner"
echo "    assumes ~1 row and can pick a catastrophic nested-loop join)"
PGPASSWORD="$ALTML_DB_PASSWORD" psql -v ON_ERROR_STOP=1 -h 127.0.0.1 -p 5432 \
    -U "$ALTML_DB_USER" -d "$ALTML_DB_NAME" -c "
    ANALYZE altml.fdw_candles_1m;
    ANALYZE altml.fdw_exchange_instruments;
    ANALYZE altml.fdw_exchanges;
    ANALYZE altml.fdw_feature_set_versions;
    ANALYZE altml.fdw_feature_set_members;
"

echo "AltML database bootstrap complete: database=${ALTML_DB_NAME} user=${ALTML_DB_USER}"
