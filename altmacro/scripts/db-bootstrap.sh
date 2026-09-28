#!/usr/bin/env bash
# Create altmacro's own database, role and schema, then apply the migrations.
#
# altmacro is a producer: it reads nothing from the other services' databases.
# Downstream consumers reach it the way the rest of the stack reaches its
# neighbours — through postgres_fdw from their own database — so nothing here
# grants altmacro access to anyone else's tables.
#
# Run as the postgres superuser on VPS1:
#   sudo -u postgres ALTMACRO_DB_PASSWORD='...' ./db-bootstrap.sh [migrations-dir]
#
# The password is passed in the environment and is never echoed. Do not put it
# on the command line, where it would be visible in the process table.
set -euo pipefail

: "${ALTMACRO_DB_PASSWORD:?set ALTMACRO_DB_PASSWORD in the environment}"
DB_NAME="${ALTMACRO_DB_NAME:-altmacro}"
DB_USER="${ALTMACRO_DB_USER:-altmacro_app}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -n "${1:-}" ]]; then
    MIGRATIONS="$1"
elif [[ -d "$SCRIPT_DIR/../db/migrations" ]]; then
    MIGRATIONS="$SCRIPT_DIR/../db/migrations"
else
    MIGRATIONS="$SCRIPT_DIR/../share/altmacro/migrations"
fi
[[ -d "$MIGRATIONS" ]] || { echo "migrations directory not found: $MIGRATIONS" >&2; exit 2; }

psql_admin() { psql -v ON_ERROR_STOP=1 "$@"; }

echo "==> role ${DB_USER}"
# The password reaches psql as a variable on stdin, never on a command line and
# never inside an interpolated SQL string: psql quotes it, so a generated
# password containing a quote cannot break out of the statement.
if psql_admin -d postgres -tAc "SELECT 1 FROM pg_roles WHERE rolname='${DB_USER}'" | grep -q 1; then
    psql_admin -d postgres -v pw="$ALTMACRO_DB_PASSWORD" <<SQL
ALTER ROLE ${DB_USER} PASSWORD :'pw';
SQL
else
    psql_admin -d postgres -v pw="$ALTMACRO_DB_PASSWORD" <<SQL
CREATE ROLE ${DB_USER} LOGIN PASSWORD :'pw';
SQL
fi

echo "==> database ${DB_NAME}"
if ! psql_admin -d postgres -tAc "SELECT 1 FROM pg_database WHERE datname='${DB_NAME}'" | grep -q 1; then
    psql_admin -d postgres -c "CREATE DATABASE ${DB_NAME} OWNER ${DB_USER}"
fi
psql_admin -d postgres -c "GRANT CONNECT ON DATABASE ${DB_NAME} TO ${DB_USER}"

echo "==> migrations from ${MIGRATIONS}"
# Each migration records itself, so re-running this script is a no-op rather
# than an error. That matters because deployment is current-only: there is no
# rollback, and the safe operation has to be the repeatable one.
psql_admin -d "$DB_NAME" -c "
    CREATE TABLE IF NOT EXISTS schema_migrations (
        version TEXT PRIMARY KEY,
        applied_at TIMESTAMPTZ NOT NULL DEFAULT now())" >/dev/null

for f in "$MIGRATIONS"/*.sql; do
    version="$(basename "$f" .sql)"
    if psql_admin -d "$DB_NAME" -tAc \
        "SELECT 1 FROM schema_migrations WHERE version='${version}'" | grep -q 1; then
        echo "    ${version}: already applied"
        continue
    fi
    echo "    ${version}: applying"
    # Each migration manages its own transactions, so it is fed to psql as-is
    # with the bookkeeping appended to the same session rather than wrapped in
    # an outer -1 transaction, which would nest and commit early.
    #
    # The residual window — process death between the migration's COMMIT and the
    # INSERT below — would make a re-run try to apply an already-applied file
    # and fail loudly on a duplicate object. That is the right failure: it stops
    # the deployment and asks for a human, rather than silently double-applying.
    { cat "$f"; printf "\nINSERT INTO schema_migrations (version) VALUES ('%s');\n" "$version"; } \
        | psql_admin -d "$DB_NAME" >/dev/null
done

echo "==> ownership"
# Everything the migrations created belongs to the service role, so the running
# process needs no superuser and no ad-hoc grants.
psql_admin -d "$DB_NAME" -tAc "
    SELECT 'ALTER TABLE '||quote_ident(schemaname)||'.'||quote_ident(tablename)||
           ' OWNER TO ${DB_USER};'
    FROM pg_tables WHERE schemaname='public'" | psql_admin -d "$DB_NAME" >/dev/null
psql_admin -d "$DB_NAME" -tAc "
    SELECT 'ALTER SEQUENCE '||quote_ident(schemaname)||'.'||quote_ident(sequencename)||
           ' OWNER TO ${DB_USER};'
    FROM pg_sequences WHERE schemaname='public'" | psql_admin -d "$DB_NAME" >/dev/null

echo "database bootstrap complete: ${DB_NAME} owned by ${DB_USER}"
