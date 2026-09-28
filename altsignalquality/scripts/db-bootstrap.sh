#!/usr/bin/env bash
# One-time, idempotent database bootstrap for AltSignalQuality.
#
# AltSignalQuality does NOT create a database of its own. Its schema lives
# inside the shared AltSpotter database, next to the `altstrategy` schema,
# because every read it makes joins its own rows to AltStrategy's proposals,
# contexts, candidates and outcomes. A separate database would put postgres_fdw
# on the hot path of both inference and every training cycle — a cost this stack
# has already measured once.
#
# This script creates the role and the schema and grants it SELECT ONLY on the
# AltStrategy tables the service reads. It never creates, alters or drops
# anything in another service's schema.
#
# Connection parameters come from the environment, never hardcoded here:
#   ALTSIGNALQUALITY_DB_NAME / _DB_USER / _DB_PASSWORD, ALTSTRATEGY_SCHEMA
set -euo pipefail

: "${ALTSIGNALQUALITY_DB_NAME:?set ALTSIGNALQUALITY_DB_NAME}"
: "${ALTSIGNALQUALITY_DB_USER:?set ALTSIGNALQUALITY_DB_USER}"
: "${ALTSIGNALQUALITY_DB_PASSWORD:?set ALTSIGNALQUALITY_DB_PASSWORD}"
ALTSTRATEGY_SCHEMA="${ALTSTRATEGY_SCHEMA:-altstrategy}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Works both from the repository tree and from an installed release, where the
# script sits in bin/ and the migrations in share/altsignalquality/db/migrations.
if [[ -n "${1:-}" ]]; then
    MIGRATIONS="$1"
elif [[ -d "$SCRIPT_DIR/../db/migrations" ]]; then
    MIGRATIONS="$SCRIPT_DIR/../db/migrations"
else
    MIGRATIONS="$SCRIPT_DIR/../share/altsignalquality/db/migrations"
fi
[[ -d "$MIGRATIONS" ]] || { echo "migrations directory not found: $MIGRATIONS" >&2; exit 2; }

psql_admin() { psql -v ON_ERROR_STOP=1 -U postgres "$@"; }

echo "==> role ${ALTSIGNALQUALITY_DB_USER}"
psql_admin -d postgres -tAc \
    "SELECT 1 FROM pg_roles WHERE rolname = '${ALTSIGNALQUALITY_DB_USER}'" | grep -q 1 || \
    psql_admin -d postgres -c \
        "CREATE ROLE ${ALTSIGNALQUALITY_DB_USER} LOGIN PASSWORD '${ALTSIGNALQUALITY_DB_PASSWORD}'"
psql_admin -d postgres -c \
    "ALTER ROLE ${ALTSIGNALQUALITY_DB_USER} PASSWORD '${ALTSIGNALQUALITY_DB_PASSWORD}'"

echo "==> altsignalquality schema inside ${ALTSIGNALQUALITY_DB_NAME}"
psql_admin -d "$ALTSIGNALQUALITY_DB_NAME" -c \
    "CREATE SCHEMA IF NOT EXISTS altsignalquality AUTHORIZATION ${ALTSIGNALQUALITY_DB_USER}"
psql_admin -d "$ALTSIGNALQUALITY_DB_NAME" -c \
    "GRANT CONNECT ON DATABASE ${ALTSIGNALQUALITY_DB_NAME} TO ${ALTSIGNALQUALITY_DB_USER}"
# search_path so unqualified names resolve inside this service's own schema
# while AltStrategy's tables stay reachable by their qualified name.
psql_admin -d "$ALTSIGNALQUALITY_DB_NAME" -c \
    "ALTER ROLE ${ALTSIGNALQUALITY_DB_USER} IN DATABASE ${ALTSIGNALQUALITY_DB_NAME}
     SET search_path = altsignalquality, public"

echo "==> read-only grants on the ${ALTSTRATEGY_SCHEMA} tables this service reads"
psql_admin -d "$ALTSIGNALQUALITY_DB_NAME" -c "
    GRANT USAGE ON SCHEMA ${ALTSTRATEGY_SCHEMA} TO ${ALTSIGNALQUALITY_DB_USER};
    GRANT SELECT ON
        ${ALTSTRATEGY_SCHEMA}.trade_signal_proposals,
        ${ALTSTRATEGY_SCHEMA}.strategy_candidates,
        ${ALTSTRATEGY_SCHEMA}.decision_context_snapshots,
        ${ALTSTRATEGY_SCHEMA}.market_outcomes,
        ${ALTSTRATEGY_SCHEMA}.proposal_warnings,
        ${ALTSTRATEGY_SCHEMA}.signal_training_examples
    TO ${ALTSIGNALQUALITY_DB_USER};
"

echo "==> migrations"
# A version table inside this service's OWN schema: the shared
# public.schema_migrations belongs to AltStrategy, and identical version numbers
# would collide there.
psql_admin -d "$ALTSIGNALQUALITY_DB_NAME" -c "
    CREATE TABLE IF NOT EXISTS altsignalquality.schema_migrations (
        version    TEXT PRIMARY KEY,
        name       TEXT NOT NULL,
        checksum   TEXT NOT NULL,
        applied_at TIMESTAMPTZ NOT NULL DEFAULT now());
    ALTER TABLE altsignalquality.schema_migrations OWNER TO ${ALTSIGNALQUALITY_DB_USER};
"
for file in $(ls -1 "$MIGRATIONS"/*.sql | sort); do
    base="$(basename "$file" .sql)"
    version="${base%%_*}"
    name="${base#*_}"
    checksum="$(sha256sum "$file" | cut -d' ' -f1)"
    applied="$(psql_admin -d "$ALTSIGNALQUALITY_DB_NAME" -tAc \
        "SELECT checksum FROM altsignalquality.schema_migrations WHERE version = '${version}'")"
    if [[ -n "$applied" ]]; then
        if [[ "$applied" != "$checksum" ]]; then
            echo "migration ${version} is already applied but its file changed" >&2
            exit 1
        fi
        echo "    ${version}_${name}: already applied"
        continue
    fi
    echo "    ${version}_${name}: applying"
    psql_admin -d "$ALTSIGNALQUALITY_DB_NAME" -1 -f "$file"
    psql_admin -d "$ALTSIGNALQUALITY_DB_NAME" -c \
        "INSERT INTO altsignalquality.schema_migrations (version, name, checksum)
         VALUES ('${version}', '${name}', '${checksum}')"
done

echo "==> schema object ownership"
psql_admin -d "$ALTSIGNALQUALITY_DB_NAME" -c "
    DO \$\$
    DECLARE r RECORD;
    BEGIN
        FOR r IN SELECT tablename FROM pg_tables WHERE schemaname = 'altsignalquality' LOOP
            EXECUTE format('ALTER TABLE altsignalquality.%I OWNER TO ${ALTSIGNALQUALITY_DB_USER}',
                           r.tablename);
        END LOOP;
        FOR r IN SELECT sequencename FROM pg_sequences WHERE schemaname = 'altsignalquality' LOOP
            EXECUTE format('ALTER SEQUENCE altsignalquality.%I OWNER TO ${ALTSIGNALQUALITY_DB_USER}',
                           r.sequencename);
        END LOOP;
    END \$\$;
"

echo "AltSignalQuality database is ready"
