#!/usr/bin/env bash
# Give a downstream service read-only access to altmacro's series through
# postgres_fdw, which is how every other pair in this stack talks.
#
# altmacro is a producer and reaches into nobody: the direction is always
# downstream-pulls. The reader role below can SELECT and nothing else, so a
# consumer cannot write, delete, or advance any of altmacro's cursors.
#
#   sudo -u postgres ./grant-downstream-fdw.sh alttech alttech_app
set -euo pipefail
CONSUMER_DB="${1:?usage: grant-downstream-fdw.sh <consumer-db> <consumer-role>}"
CONSUMER_ROLE="${2:?usage: grant-downstream-fdw.sh <consumer-db> <consumer-role>}"
SRC_DB="${ALTMACRO_DB_NAME:-altmacro}"
READER="${ALTMACRO_READER_ROLE:-altmacro_reader}"
SCHEMA="${ALTMACRO_FOREIGN_SCHEMA:-altmacro_ref}"

psql_admin() { psql -v ON_ERROR_STOP=1 "$@"; }

# The reader's password is generated here and never printed. It exists only in
# the two places that need it: the role and the user mapping.
PW="$(head -c 32 /dev/urandom | base64 | tr -d '/+=' | head -c 32)"

echo "==> read-only role ${READER} in ${SRC_DB}"
if psql_admin -d "$SRC_DB" -tAc "SELECT 1 FROM pg_roles WHERE rolname='${READER}'" | grep -q 1; then
    psql_admin -d "$SRC_DB" -v pw="$PW" <<SQL
ALTER ROLE ${READER} PASSWORD :'pw';
SQL
else
    psql_admin -d "$SRC_DB" -v pw="$PW" <<SQL
CREATE ROLE ${READER} LOGIN PASSWORD :'pw';
SQL
fi

psql_admin -d "$SRC_DB" <<SQL
GRANT CONNECT ON DATABASE ${SRC_DB} TO ${READER};
GRANT USAGE ON SCHEMA public TO ${READER};
GRANT SELECT ON ALL TABLES IN SCHEMA public TO ${READER};
-- Tables created later (monthly bar partitions) must be readable too, or the
-- consumer would silently stop seeing new months.
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT SELECT ON TABLES TO ${READER};
SQL

echo "==> foreign server and mapping in ${CONSUMER_DB}"
psql_admin -d "$CONSUMER_DB" <<SQL
CREATE EXTENSION IF NOT EXISTS postgres_fdw;
DO \$\$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_foreign_server WHERE srvname='altmacro_srv') THEN
        CREATE SERVER altmacro_srv FOREIGN DATA WRAPPER postgres_fdw
            OPTIONS (host '127.0.0.1', port '5432', dbname '${SRC_DB}',
                     -- Without this the planner assumes fixed costs for every
                     -- foreign table and picks catastrophic plans on the bar
                     -- partitions. The rest of this stack learned that the
                     -- hard way.
                     use_remote_estimate 'true', fetch_size '10000');
    END IF;
END
\$\$;
GRANT USAGE ON FOREIGN SERVER altmacro_srv TO ${CONSUMER_ROLE};
SQL

psql_admin -d "$CONSUMER_DB" -v pw="$PW" <<SQL
DROP USER MAPPING IF EXISTS FOR ${CONSUMER_ROLE} SERVER altmacro_srv;
CREATE USER MAPPING FOR ${CONSUMER_ROLE} SERVER altmacro_srv
    OPTIONS (user '${READER}', password :'pw');
DROP USER MAPPING IF EXISTS FOR CURRENT_USER SERVER altmacro_srv;
CREATE USER MAPPING FOR CURRENT_USER SERVER altmacro_srv
    OPTIONS (user '${READER}', password :'pw');
SQL
unset PW

echo "==> importing into schema ${SCHEMA}"
# The schema belongs to the consumer, because IMPORT FOREIGN SCHEMA runs as the
# mapped role and has to create the foreign tables inside it. Ownership keeps
# that contained: the consumer manages its own view of altmacro and can re-import
# after a schema change without needing a superuser.
psql_admin -d "$CONSUMER_DB" <<SQL
CREATE SCHEMA IF NOT EXISTS ${SCHEMA} AUTHORIZATION ${CONSUMER_ROLE};
ALTER SCHEMA ${SCHEMA} OWNER TO ${CONSUMER_ROLE};
GRANT USAGE ON SCHEMA ${SCHEMA} TO ${CONSUMER_ROLE};
SQL

# IMPORT FOREIGN SCHEMA runs as the mapped user, so it is performed under the
# consumer's role rather than as postgres -- otherwise there is no mapping for
# the session and the import fails with "user mapping not found".
psql_admin -d "$CONSUMER_DB" <<SQL
SET ROLE ${CONSUMER_ROLE};
IMPORT FOREIGN SCHEMA public
    LIMIT TO (macro_instruments, macro_providers, macro_instrument_sources,
              macro_price_bars, macro_reference_values, macro_events,
              macro_series_gaps)
    FROM SERVER altmacro_srv INTO ${SCHEMA};
RESET ROLE;
GRANT SELECT ON ALL TABLES IN SCHEMA ${SCHEMA} TO ${CONSUMER_ROLE};
SQL

echo "downstream access granted: ${CONSUMER_DB}.${SCHEMA} -> ${SRC_DB} (read-only)"
